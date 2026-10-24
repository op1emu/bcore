#include "cp_runtime.h"

#include <chrono>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include "compiler.h"

using namespace llvm;

namespace bcore_cp {

namespace {

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

// A packet's lift, between lifting and keeping its code.
struct RuntimeStencils::Lifted {
    Packet packet;
    std::string name;
    uint16_t flags = 0;
    uint8_t bytes = 0;
    std::string error;
};

bool RuntimeStencils::Packet::operator==(const Packet& o) const {
    if (count != o.count) return false;
    for (unsigned i = 0; i < count; ++i)
        if (words[i] != o.words[i]) return false;
    return true;
}

uint64_t RuntimeStencils::key_hash(const Packet& packet) {
    // splitmix64 over the halfwords and their number
    uint64_t x = 0x9e3779b97f4a7c15ull * (packet.count + 1);
    for (unsigned i = 0; i < packet.count; ++i) {
        x += packet.words[i];
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        x ^= x >> 31;
    }
    return x;
}

RuntimeStencils::RuntimeStencils() = default;
RuntimeStencils::~RuntimeStencils() = default;

bool RuntimeStencils::configure(const Settings& settings) {
    // What is kept was built with the old settings.
    discard();
    entries_.clear();
    records_.clear();
    const bool rebuild = !compiler_ || settings.opt != settings_.opt;
    settings_ = settings;
    if (!rebuild) return true;
    translator_.reset();
    compiler_ = std::make_unique<Compiler>();
    if (!compiler_->init(settings.opt, settings.opt, /*split=*/false, /*stencil=*/true)) {
        compiler_.reset();
        return false;
    }
    context_ = std::make_unique<LLVMContext>();
    memory_ = std::make_unique<StencilMemory>();
    // As tools/stencilgen.cpp's: the stencils' blocks keep their packet
    // state as the LLVM tier 0's do (Core::set_lazy_state, on by default).
    translator_ = std::make_unique<BBTranslator>(memory_.get());
    translator_->set_lazy_state(true);
    return true;
}

RuntimeStencils::Found RuntimeStencils::find(const Packet& packet) const {
    Found out;
    auto it = entries_.find(packet);
    if (it == entries_.end()) return out;
    out.record = it->second.record;
    out.more = it->second.more;
    out.refused = !out.record && !out.more;
    if (!out.more) stats_.hits++;
    return out;
}

void RuntimeStencils::refuse(const Packet& packet, const std::string& why) {
    entries_[packet] = Entry{nullptr, false};
    stats_.refused++;
    stats_.reasons[why]++;
}

std::string RuntimeStencils::lift(Module& mod, Packet& packet, const std::string& name, Lifted& out,
                                  const std::function<uint16_t(int)>& fetch) {
    // As the build lifts and checks its packets (tools/stencilgen.cpp),
    // but without field holes: the run time builds each encoding's own.
    auto holes = [](Module& m) {
        auto h = packet_holes(m);
        for (auto*& field : h.fields) field = nullptr;
        return h;
    };
    out.name = name;
    memory_->set_words(kPcA, packet.words, packet.count, fetch);
    BBTranslator::StencilLift a;
    try {
        a = translator_->translate_stencil(mod, name.c_str(), kPcA, holes(mod));
    } catch (...) {
        // A fetch threw (the translator's Memory): no function left behind.
        memory_->set_words(kPcA, nullptr, 0);
        if (auto* fn = mod.getFunction(name)) fn->eraseFromParent();
        throw;
    }
    // The halfwords the decoder read.
    packet.count = uint8_t(memory_->count());
    for (int i = 0; i < packet.count; ++i) packet.words[i] = memory_->words()[i];
    out.packet = packet;
    const int len = 2 * packet.count;
    std::string error;
    if (memory_->read_outside()) {
        error = "reads past the packet";
    } else if (a.bytes > len || (a.bytes < len && !a.terminated)) {
        // Shorter only where it ends the block: an illegal instruction
        // raises its exception as a packet of 2, whatever it read.
        error = "decodes to another length";
    } else if (a.fields_used) {
        error = "takes a field hole";
    } else {
        // The same IR at another PC: nothing in it depends on the PC but
        // the PC hole. Lifted beside it, in its module, and compared there
        // (same_ir: comparing the two printed took more than a third of the
        // run time's stencils, as much as compiling them).
        memory_->set_words(kPcB, packet.words, packet.count);
        auto b = translator_->translate_stencil(mod, (name + ".b").c_str(), kPcB, holes(mod));
        if (memory_->read_outside()) error = "reads past the packet";
        else if (!same_ir(*a.fn, *b.fn)) error = "IR depends on the PC";
        else if (verifyFunction(*a.fn, &errs())) error = "IR does not verify";
        b.fn->eraseFromParent();
    }
    if (!error.empty()) {
        a.fn->eraseFromParent();
        return error;
    }
    out.bytes = uint8_t(a.bytes);
    out.flags = (a.terminated ? kTerminates : 0) | (a.cycles ? kCycles : 0) |
                (packet.count == 1 && packet.words[0] == 0x0020 ? kIdle : 0);
    return {};
}

bool RuntimeStencils::compile(Module& mod, std::map<std::string, Code>& code) {
    const uint64_t t0 = now_ns();
    // As Worker::compile in tools/stencilgen.cpp.
    mod.setDataLayout(compiler_->dataLayout());
    mod.setTargetTriple(compiler_->targetTriple());
    make_local(mod);
    compiler_->optimize(mod);
    auto object = compiler_->emit(mod);
    const uint64_t t1 = now_ns();
    if (object) code = extract(*object);
    stats_.compile_ns += t1 - t0;
    stats_.extract_ns += now_ns() - t1;
    stats_.batches++;
    return object != nullptr;
}

RuntimeStencils::Staged RuntimeStencils::stage(Packet& packet, const std::function<uint16_t(int)>& fetch) {
    Staged out;
    if (!compiler_ || find(packet).known()) return out;
    for (const Lifted& l : pending_)
        if (l.packet == packet) return {true, l.bytes, l.flags};
    if (packet.count < 1 || packet.count > StencilMemory::kMaxWords) {
        refuse(packet, "not a packet");
        return out;
    }
    if (!batch_) batch_ = std::make_unique<Module>("stencils", *context_);
    const uint64_t t0 = now_ns();
    Lifted l;
    const Packet first = packet;
    std::string error = lift(*batch_, packet, "rt_" + std::to_string(pending_.size()), l, fetch);
    stats_.lift_ns += now_ns() - t0;
    // The halfwords the decoding read before its last: each says read one
    // more.
    for (Packet prefix = first; prefix.count < packet.count; ++prefix.count) {
        entries_[prefix] = Entry{nullptr, true};
        prefix.words[prefix.count] = packet.words[prefix.count];
    }
    // The range and the room are the whole packet's (found in review: the
    // range took a parallel packet's first two halfwords for it, and a
    // refusal kept there refused every packet that starts with them).
    const uint64_t h = key_hash(packet);
    if (error.empty() && (h < settings_.range_lo || h > settings_.range_hi)) error = "outside the range";
    else if (error.empty() && records_.size() + pending_.size() >= settings_.capacity) error = "no room";
    if (!error.empty()) {
        if (auto* fn = batch_->getFunction(l.name)) fn->eraseFromParent();
        refuse(packet, error);
    } else {
        out = {true, l.bytes, l.flags};
        pending_.push_back(std::move(l));
    }
    return out;
}

void RuntimeStencils::discard() {
    pending_.clear();
    batch_.reset();
}

void RuntimeStencils::flush() {
    if (pending_.empty()) return;
    std::vector<Lifted> lifted = std::move(pending_);
    std::unique_ptr<Module> mod = std::move(batch_);
    discard();
    std::map<std::string, Code> code;
    if (!compile(*mod, code)) {
        for (const Lifted& l : lifted) refuse(l.packet, "no object");
        return;
    }
    mod.reset();
    for (Lifted& l : lifted) {
        auto it = code.find(l.name);
        if (it == code.end()) {
            refuse(l.packet, "no code");
            continue;
        }
        Code& c = it->second;
        if (!c.error.empty()) {
            refuse(l.packet, c.error);
            continue;
        }
        if (c.relocs.size() > 0xffff) {
            refuse(l.packet, "too many relocations");
            continue;
        }
        if (settings_.verify) {
            // Its code on its own: the same as in the batch.
            Module alone("alone", *context_);
            Lifted again;
            Packet packet = l.packet;
            std::map<std::string, Code> own;
            const bool same = lift(alone, packet, l.name, again).empty() && compile(alone, own) &&
                              own.count(l.name) && own[l.name].bytes == c.bytes &&
                              own[l.name].relocs.size() == c.relocs.size() &&
                              std::equal(c.relocs.begin(), c.relocs.end(), own[l.name].relocs.begin(),
                                         [](const Reloc& x, const Reloc& y) {
                                             return x.offset == y.offset && x.hole == y.hole && x.kind == y.kind &&
                                                    x.addend == y.addend;
                                         });
            if (!same) {
                refuse(l.packet, "verify: its code depends on its batch");
                continue;
            }
        }
        Record& r = records_.emplace_back();
        r.stencil.size = uint16_t(c.bytes.size());
        r.stencil.flags = uint16_t(l.flags | code_flags(c));
        r.stencil.reloc_count = uint16_t(c.relocs.size());
        r.stencil.bytes = l.bytes;
        r.code = std::move(c.bytes);
        r.relocs = std::move(c.relocs);
        entries_[l.packet] = Entry{&r, false};
        stats_.built++;
        stats_.code_bytes += r.code.size();
    }
}

}  // namespace bcore_cp

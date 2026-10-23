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

uint64_t RuntimeStencils::Packet::key() const {
    uint64_t k = 0;
    for (unsigned i = 0; i < count; ++i) k = k << 16 | words[i];
    return k;
}

uint64_t RuntimeStencils::key_hash(const Packet& packet) {
    // splitmix64 of the encoding and its length
    uint64_t x = packet.key() + 0x9e3779b97f4a7c15ull * (packet.count + 1);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

RuntimeStencils::RuntimeStencils() = default;
RuntimeStencils::~RuntimeStencils() = default;

bool RuntimeStencils::configure(const Settings& settings) {
    // What is kept was built with the old settings.
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

const RuntimeStencils::Record* RuntimeStencils::find(const Packet& packet) const {
    auto it = entries_.find(key_of(packet));
    if (it == entries_.end()) return nullptr;
    stats_.hits++;
    return it->second.record;
}

bool RuntimeStencils::tried(const Packet& packet) const { return entries_.count(key_of(packet)) != 0; }

void RuntimeStencils::refuse(const Packet& packet, const std::string& why) {
    entries_[key_of(packet)] = Entry{nullptr};
    stats_.refused++;
    stats_.reasons[why]++;
}

bool RuntimeStencils::compile(const std::vector<Packet>& packets, std::vector<Lifted>& lifted,
                              std::map<std::string, Code>& code) {
    Module mod("stencils", *context_);
    const uint64_t t0 = now_ns();
    for (const Packet& packet : packets) {
        Lifted l;
        l.packet = packet;
        l.name = "rt_" + std::to_string(lifted.size());
        const int len = 2 * packet.count;
        // As the build checks its packets (tools/stencilgen.cpp): lifted at
        // two PCs, the same IR, nothing in it depending on the PC but the PC
        // hole. The lift at kPcA is the one compiled.
        memory_->set_words(kPcA, packet.words, packet.count);
        auto a = translator_->translate_stencil(mod, l.name.c_str(), kPcA, packet_holes(mod));
        const bool outside = memory_->read_outside();
        if (outside) {
            l.error = "reads past the packet";
        } else if (a.bytes != len && !(a.terminated && a.bytes < len)) {
            l.error = "decodes to another length";  // an illegal instruction raises its exception as a packet of 2
        } else {
            Module scratch("scratch", *context_);
            memory_->set_words(kPcB, packet.words, packet.count);
            // Its own module: the same name as the lift at kPcA.
            auto b = translator_->translate_stencil(scratch, l.name.c_str(), kPcB, packet_holes(scratch));
            if (memory_->read_outside()) l.error = "reads past the packet";
            else if (ir_text(b.fn) != ir_text(a.fn)) l.error = "IR depends on the PC";
            else if (verifyFunction(*a.fn, &errs()))
                l.error = "IR does not verify";
        }
        if (!l.error.empty()) {
            a.fn->eraseFromParent();
        } else {
            l.bytes = uint8_t(a.bytes);
            l.flags = (a.terminated ? kTerminates : 0) | (a.cycles ? kCycles : 0) |
                      (packet.count == 1 && packet.words[0] == 0x0020 ? kIdle : 0);
        }
        lifted.push_back(std::move(l));
    }
    const uint64_t t1 = now_ns();
    // As Worker::compile in tools/stencilgen.cpp.
    mod.setDataLayout(compiler_->dataLayout());
    mod.setTargetTriple(compiler_->targetTriple());
    make_local(mod);
    compiler_->optimize(mod);
    auto object = compiler_->emit(mod);
    const uint64_t t2 = now_ns();
    if (object) code = extract(*object);
    const uint64_t t3 = now_ns();
    stats_.lift_ns += t1 - t0;
    stats_.compile_ns += t2 - t1;
    stats_.extract_ns += t3 - t2;
    stats_.batches++;
    return object != nullptr;
}

void RuntimeStencils::build(const std::vector<Packet>& packets) {
    if (!compiler_) return;
    std::vector<Packet> todo;
    for (const Packet& packet : packets) {
        if (tried(packet)) continue;
        bool twice = false;
        for (const Packet& t : todo) twice |= key_of(t) == key_of(packet);
        if (twice) continue;
        const uint64_t h = key_hash(packet);
        if (packet.count != 1 && packet.count != 2 && packet.count != 4)
            refuse(packet, "not a packet");
        else if (h < settings_.range_lo || h > settings_.range_hi)
            refuse(packet, "outside the range");
        else if (records_.size() + todo.size() >= settings_.capacity)
            refuse(packet, "no room");
        else
            todo.push_back(packet);
    }
    if (todo.empty()) return;

    std::vector<Lifted> lifted;
    std::map<std::string, Code> code;
    if (!compile(todo, lifted, code)) {
        for (const Packet& packet : todo) refuse(packet, "no object");
        return;
    }
    for (Lifted& l : lifted) {
        if (!l.error.empty()) {
            refuse(l.packet, l.error);
            continue;
        }
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
            std::vector<Lifted> alone;
            std::map<std::string, Code> own;
            const bool same = compile({l.packet}, alone, own) && alone[0].error.empty() && own.count(alone[0].name) &&
                              own[alone[0].name].bytes == c.bytes &&
                              own[alone[0].name].relocs.size() == c.relocs.size() &&
                              std::equal(c.relocs.begin(), c.relocs.end(), own[alone[0].name].relocs.begin(),
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
        r.stencil.flags = uint16_t(l.flags | (tail_cont(c) ? kTailCont : 0));
        r.stencil.reloc_count = uint16_t(c.relocs.size());
        r.stencil.bytes = l.bytes;
        r.code = std::move(c.bytes);
        r.relocs = std::move(c.relocs);
        entries_[key_of(l.packet)] = Entry{&r};
        stats_.built++;
        stats_.code_bytes += r.code.size();
    }
}

}  // namespace bcore_cp

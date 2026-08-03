// Unit tests for the Mirage IR (src/compiler/mir.{hpp,cpp}) — the builder, the verifier
// and the printer.
//
// The verifier is the load-bearing part: it is what the native backends will run on every
// compile, the analogue of llvm::verifyModule (which has already caught real bugs in the
// LLVM path). So most of what follows builds a DELIBERATELY BROKEN module and checks that
// the verifier notices — a verifier that accepts everything is worse than none, because it
// reads as a guarantee.

#include "compiler/mir.hpp"

#include <cstdio>
#include <string>

namespace {
    int failures = 0;

    void check(const bool condition, const std::string &what) {
        if (condition) {
            std::printf("ok: %s\n", what.c_str());
        } else {
            ++failures;
            std::printf("FAIL: %s\n", what.c_str());
        }
    }

    // Whether any verifier message contains 'fragment'. Substring rather than equality so a
    // reworded diagnostic does not break every test.
    auto has_error(const std::vector<mir::VerifyError> &errors, const std::string &fragment) -> bool {
        for (const auto &error : errors) {
            if (error.message.find(fragment) != std::string::npos) return true;
        }
        return false;
    }

    auto describe(const std::vector<mir::VerifyError> &errors) -> std::string {
        std::string out;
        for (const auto &error : errors) {
            out += "\n    " + error.message;
        }
        return out.empty() ? std::string(" (none)") : out;
    }

    // A module with one function 'f(i64) -> i64' and an empty entry block, ready to have a
    // body appended. Returns the builder positioned in the entry block.
    struct Fixture {
        mir::Module module;
        uint32_t fn_index = 0;

        explicit Fixture(const mir::Ty result = mir::Ty::I64,
                          const std::vector<mir::Ty> &params = {mir::Ty::I64}) {
            module.name = "test";
            const auto sig = module.intern_signature(mir::Signature{.params = params, .result = result});
            module.functions.push_back(mir::Function{
                .name = "f",
                .signature = sig,
                .has_body = true,
            });
            fn_index = 0;
        }

        auto builder() -> mir::Builder { return mir::Builder(module, fn_index); }
    };

    // ---------------------------------------------------------------- well-formed

    void test_builds_and_verifies_a_simple_function() {
        Fixture fixture;
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        const auto param = b.add_block_param(entry, mir::Ty::I64);
        fixture.module.functions[0].params.push_back(param);
        b.set_insert_point(entry);

        const auto one = b.const_int(mir::Ty::I64, 1);
        const auto sum = b.binary(mir::Op::Add, mir::Ty::I64, param, one);
        b.ret(sum);

        const auto errors = mir::verify(fixture.module);
        check(errors.empty(), "a well-formed function verifies clean:" + describe(errors));

        const auto text = mir::print(fixture.module);
        check(text.find("fn @f(%0: i64) -> i64") != std::string::npos, "the printer renders the signature");
        check(text.find("add") != std::string::npos, "the printer renders the arithmetic");
        check(text.find("return") != std::string::npos, "the printer renders the terminator");

        // Determinism: the whole point of a textual form is that it can be diffed.
        check(mir::print(fixture.module) == text, "printing is deterministic");
    }

    void test_block_params_replace_phi() {
        // if (c) { x = 1 } else { x = 2 }; return x  — the shape that needed a phi node.
        Fixture fixture(mir::Ty::I64, {mir::Ty::I1});
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        const auto cond = b.add_block_param(entry, mir::Ty::I1);
        fixture.module.functions[0].params.push_back(cond);
        const auto then_block = b.create_block("then");
        const auto else_block = b.create_block("else");
        const auto join = b.create_block("join");
        const auto merged = b.add_block_param(join, mir::Ty::I64);

        b.set_insert_point(entry);
        b.branch(cond, then_block, else_block);

        b.set_insert_point(then_block);
        b.jump(join, {b.const_int(mir::Ty::I64, 1)});

        b.set_insert_point(else_block);
        b.jump(join, {b.const_int(mir::Ty::I64, 2)});

        b.set_insert_point(join);
        b.ret(merged);

        const auto errors = mir::verify(fixture.module);
        check(errors.empty(), "a block-parameter merge verifies clean:" + describe(errors));
        // Block references carry the block INDEX as well as the label, because labels are
        // an emitter-chosen readability aid and are freely duplicated (every nested 'if'
        // produces an "if.end"). A label-only reference is ambiguous exactly where control
        // flow is hardest to follow.
        check(mir::print(fixture.module).find("^join.3(%") != std::string::npos,
              "the printer renders block parameters, with an unambiguous block reference");
    }

    void test_slots_and_memory() {
        Fixture fixture(mir::Ty::I64, {});
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);

        const auto slot = b.add_slot(16, 8, "local");
        const auto base = b.slot_addr(slot);
        b.store(base, b.const_int(mir::Ty::I64, 7));
        // Field at offset 8: plain pointer arithmetic, because sema computed the offset.
        const auto second = b.ptr_add_const(base, 8);
        b.store(second, b.const_int(mir::Ty::I64, 9));
        b.ret(b.binary(mir::Op::Add, mir::Ty::I64, b.load(mir::Ty::I64, base), b.load(mir::Ty::I64, second)));

        const auto errors = mir::verify(fixture.module);
        check(errors.empty(), "slots, stores and offset loads verify clean:" + describe(errors));
    }

    void test_ptr_add_const_folds_zero() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);
        const auto base = b.slot_addr(b.add_slot(8, 8, "s"));
        const auto same = b.ptr_add_const(base, 0);
        b.ret();

        check(same == base, "a zero-offset ptr.add.const folds away (field 0 is the common case)");
        check(mir::print(fixture.module).find("ptr.add.const") == std::string::npos,
              "and emits no instruction");
    }

    void test_signature_interning() {
        mir::Module module;
        const auto a = module.intern_signature(mir::Signature{.params = {mir::Ty::I32}, .result = mir::Ty::I32});
        const auto b = module.intern_signature(mir::Signature{.params = {mir::Ty::I32}, .result = mir::Ty::I32});
        const auto c = module.intern_signature(mir::Signature{.params = {mir::Ty::I64}, .result = mir::Ty::I32});
        check(a == b, "identical signatures intern to one index (wasm needs unique type indices)");
        check(a != c, "differing signatures do not");
        check(module.signatures.size() == 2, "only the distinct ones are stored");
    }

    // ---------------------------------------------------------------- verifier

    void test_rejects_block_without_terminator() {
        Fixture fixture;
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        fixture.module.functions[0].params.push_back(b.add_block_param(entry, mir::Ty::I64));
        b.set_insert_point(entry);
        b.const_int(mir::Ty::I64, 1);   // and nothing else

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "does not end in a terminator"), "a block with no terminator is rejected");
    }

    void test_rejects_terminator_in_the_middle() {
        // The single most common emitter bug: writing into a block that was already closed.
        Fixture fixture;
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        const auto param = b.add_block_param(entry, mir::Ty::I64);
        fixture.module.functions[0].params.push_back(param);
        b.set_insert_point(entry);
        b.ret(param);
        b.ret(param);   // emitted into a closed block

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "but is not last"), "a terminator in the middle of a block is rejected");
    }

    void test_rejects_empty_block() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);
        b.ret();
        b.create_block("orphan");   // never filled in

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "no instructions"), "a block left empty is rejected");
    }

    void test_rejects_out_of_range_branch_target() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);
        b.jump(99);

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "out of range"), "a branch to a nonexistent block is rejected");
    }

    void test_rejects_block_argument_mismatch() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        const auto target = b.create_block("target");
        b.add_block_param(target, mir::Ty::I64);

        b.set_insert_point(entry);
        b.jump(target);            // arity mismatch: the target takes one parameter
        b.set_insert_point(target);
        b.ret();

        auto errors = mir::verify(fixture.module);
        check(has_error(errors, "argument(s) were passed"), "block-argument arity is checked");

        // Now the right count but the wrong type.
        Fixture typed(mir::Ty::Void, {});
        auto b2 = typed.builder();
        const auto e2 = b2.create_block("entry");
        const auto t2 = b2.create_block("target");
        b2.add_block_param(t2, mir::Ty::I64);
        b2.set_insert_point(e2);
        b2.jump(t2, {b2.const_int(mir::Ty::I32, 1)});
        b2.set_insert_point(t2);
        b2.ret();

        errors = mir::verify(typed.module);
        check(has_error(errors, "but parameter is"), "block-argument types are checked");
    }

    void test_rejects_type_mismatched_operands() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);
        // i64 + i32 — the two operands of an integer binary must share the result type.
        b.binary(mir::Op::Add, mir::Ty::I64, b.const_int(mir::Ty::I64, 1), b.const_int(mir::Ty::I32, 2));
        b.ret();

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "expected 'i64'"), "mismatched arithmetic operand types are rejected");
    }

    void test_rejects_wrong_return_shape() {
        Fixture returns_value(mir::Ty::I64, {});
        auto b = returns_value.builder();
        b.set_insert_point(b.create_block("entry"));
        b.ret();   // returns nothing from a function that must return i64

        auto errors = mir::verify(returns_value.module);
        check(has_error(errors, "0 value(s) were returned"), "returning nothing from a value function is rejected");

        Fixture returns_void(mir::Ty::Void, {});
        auto b2 = returns_void.builder();
        b2.set_insert_point(b2.create_block("entry"));
        b2.ret(b2.const_int(mir::Ty::I64, 1));

        errors = mir::verify(returns_void.module);
        check(has_error(errors, "returns void but a value was returned"),
              "returning a value from a void function is rejected");
    }

    void test_rejects_non_pointer_load() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        b.set_insert_point(b.create_block("entry"));
        b.load(mir::Ty::I64, b.const_int(mir::Ty::I64, 0));   // loading through an integer
        b.ret();

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "expected 'ptr'"), "loading through a non-pointer is rejected");
    }

    void test_rejects_non_i1_branch_condition() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        const auto entry = b.create_block("entry");
        const auto yes = b.create_block("yes");
        const auto no = b.create_block("no");
        b.set_insert_point(entry);
        b.branch(b.const_int(mir::Ty::I32, 1), yes, no);
        b.set_insert_point(yes);
        b.ret();
        b.set_insert_point(no);
        b.ret();

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "expected 'i1'"), "a non-i1 branch condition is rejected");
    }

    void test_rejects_call_arity_and_type_errors() {
        mir::Module module;
        module.name = "test";
        const auto callee_sig = module.intern_signature(
            mir::Signature{.params = {mir::Ty::I32, mir::Ty::I32}, .result = mir::Ty::I32});
        module.functions.push_back(mir::Function{.name = "callee", .signature = callee_sig, .has_body = false});

        const auto caller_sig = module.intern_signature(mir::Signature{.result = mir::Ty::Void});
        module.functions.push_back(mir::Function{.name = "caller", .signature = caller_sig, .has_body = true});

        mir::Builder b(module, 1);
        b.set_insert_point(b.create_block("entry"));
        const auto arg = b.const_int(mir::Ty::I32, 1);
        b.call(0, mir::Ty::I32, {arg});   // one argument, but 'callee' takes two
        b.ret();

        auto errors = mir::verify(module);
        check(has_error(errors, "takes 2 argument(s), 1 passed"), "call arity is checked against the callee");

        // Right arity, wrong argument type.
        module.functions[1].blocks.clear();
        module.functions[1].values.clear();
        mir::Builder b2(module, 1);
        b2.set_insert_point(b2.create_block("entry"));
        b2.call(0, mir::Ty::I32, {b2.const_int(mir::Ty::I32, 1), b2.const_int(mir::Ty::I64, 2)});
        b2.ret();

        errors = mir::verify(module);
        check(has_error(errors, "expected 'i32'"), "call argument types are checked");

        // Right arity and types, wrong result type on the call itself.
        module.functions[1].blocks.clear();
        module.functions[1].values.clear();
        mir::Builder b3(module, 1);
        b3.set_insert_point(b3.create_block("entry"));
        b3.call(0, mir::Ty::I64, {b3.const_int(mir::Ty::I32, 1), b3.const_int(mir::Ty::I32, 2)});
        b3.ret();

        errors = mir::verify(module);
        check(has_error(errors, "but the call is typed"), "the call's result type is checked");
    }

    void test_rejects_dangling_references() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        b.set_insert_point(b.create_block("entry"));
        b.slot_addr(42);        // no such slot
        b.global_addr(7);       // no such global
        b.ret();

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "slot 42 does not exist"), "a reference to a nonexistent slot is rejected");
        check(has_error(errors, "global 7 does not exist"), "a reference to a nonexistent global is rejected");
    }

    void test_rejects_identity_conversion() {
        Fixture fixture(mir::Ty::Void, {});
        auto b = fixture.builder();
        b.set_insert_point(b.create_block("entry"));
        b.convert(mir::Op::ZExt, mir::Ty::I32, b.const_int(mir::Ty::I32, 1));
        b.ret();

        const auto errors = mir::verify(fixture.module);
        check(has_error(errors, "converts 'i32' to itself"),
              "a conversion that changes nothing is rejected (a sign the emitter lost track)");
    }

    void test_rejects_declaration_with_blocks() {
        mir::Module module;
        module.name = "test";
        const auto sig = module.intern_signature(mir::Signature{.result = mir::Ty::Void});
        module.functions.push_back(mir::Function{.name = "decl", .signature = sig, .has_body = false});
        module.functions[0].blocks.push_back(mir::Block{.label = "entry"});

        const auto errors = mir::verify(module);
        check(has_error(errors, "is a declaration but has blocks"), "a declaration with a body is rejected");
    }

    // ---- promote_slots (stage 3) --------------------------------------------------

    void test_promote_slots_straight_line() {
        Fixture f;
        mir::Builder b(f.module, f.fn_index);
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);
        const auto arg = b.add_block_param(entry, mir::Ty::I64);
        const auto slot = b.add_slot(8, 8, "x");
        b.store(b.slot_addr(slot), arg);
        const auto loaded = b.load(mir::Ty::I64, b.slot_addr(slot));
        b.ret(b.binary(mir::Op::Add, mir::Ty::I64, loaded, b.const_int(mir::Ty::I64, 2)));

        const auto stats = mir::promote_slots(f.module);
        const auto errors = mir::verify(f.module);
        check(errors.empty(), "straight-line promotion verifies" + describe(errors));
        check(stats.slots_promoted == 1, "the slot is promoted");
        check(f.module.functions[0].slots.empty(), "and removed from the frame");
        const auto text = mir::print(f.module);
        check(text.find("load") == std::string::npos && text.find("store") == std::string::npos,
              "no loads or stores remain");
    }

    void test_promote_slots_diamond_becomes_block_param() {
        Fixture f(mir::Ty::I64, {mir::Ty::I1});
        mir::Builder b(f.module, f.fn_index);
        const auto entry = b.create_block("entry");
        const auto then_block = b.create_block("then");
        const auto else_block = b.create_block("else");
        const auto join = b.create_block("join");
        b.set_insert_point(entry);
        const auto cond = b.add_block_param(entry, mir::Ty::I1);
        const auto slot = b.add_slot(8, 8, "x");
        b.branch(cond, then_block, else_block);
        b.set_insert_point(then_block);
        b.store(b.slot_addr(slot), b.const_int(mir::Ty::I64, 1));
        b.jump(join);
        b.set_insert_point(else_block);
        b.store(b.slot_addr(slot), b.const_int(mir::Ty::I64, 2));
        b.jump(join);
        b.set_insert_point(join);
        b.ret(b.load(mir::Ty::I64, b.slot_addr(slot)));

        const auto stats = mir::promote_slots(f.module);
        const auto errors = mir::verify(f.module);
        check(errors.empty(), "diamond promotion verifies" + describe(errors));
        check(stats.slots_promoted == 1 && stats.params_added == 1,
              "the merge becomes one block parameter");
        const auto text = mir::print(f.module);
        check(text.find("^join") != std::string::npos && text.find("(%") != std::string::npos,
              "the join block takes the value as a parameter");
    }

    void test_promote_slots_loop_counter() {
        // i = 0; while (i < n) i = i + 1; return i  -- the counter's merge sits on a
        // loop header whose predecessors include its own back edge, the cyclic case
        // the eager memoization exists for. The header is a BRANCH target, so its new
        // parameter forces the conditional edge through a jump-only trampoline.
        Fixture f;
        mir::Builder b(f.module, f.fn_index);
        const auto entry = b.create_block("entry");
        const auto header = b.create_block("header");
        const auto body = b.create_block("body");
        const auto exit = b.create_block("exit");
        b.set_insert_point(entry);
        const auto n = b.add_block_param(entry, mir::Ty::I64);
        const auto slot = b.add_slot(8, 8, "i");
        b.store(b.slot_addr(slot), b.const_int(mir::Ty::I64, 0));
        b.jump(header);
        b.set_insert_point(header);
        const auto i1 = b.load(mir::Ty::I64, b.slot_addr(slot));
        b.branch(b.compare(mir::Op::ICmpSlt, i1, n), body, exit);
        b.set_insert_point(body);
        const auto i2 = b.load(mir::Ty::I64, b.slot_addr(slot));
        b.store(b.slot_addr(slot), b.binary(mir::Op::Add, mir::Ty::I64, i2, b.const_int(mir::Ty::I64, 1)));
        b.jump(header);
        b.set_insert_point(exit);
        b.ret(b.load(mir::Ty::I64, b.slot_addr(slot)));

        const auto stats = mir::promote_slots(f.module);
        const auto errors = mir::verify(f.module);
        check(errors.empty(), "loop promotion verifies" + describe(errors));
        check(stats.slots_promoted == 1, "the counter slot is promoted");
        check(f.module.functions[0].slots.empty(), "and leaves no frame slots");
    }

    void test_promote_slots_splits_branch_edges() {
        // 'if (c) x = 1; return x' -- the join is reached DIRECTLY by one arm of the
        // branch, and a branch target cannot carry block arguments (the verifier's
        // rule), so that edge must be split through a jump-only trampoline.
        Fixture f(mir::Ty::I64, {mir::Ty::I1});
        mir::Builder b(f.module, f.fn_index);
        const auto entry = b.create_block("entry");
        const auto then_block = b.create_block("then");
        const auto join = b.create_block("join");
        b.set_insert_point(entry);
        const auto cond = b.add_block_param(entry, mir::Ty::I1);
        const auto slot = b.add_slot(8, 8, "x");
        b.store(b.slot_addr(slot), b.const_int(mir::Ty::I64, 7));
        b.branch(cond, then_block, join);
        b.set_insert_point(then_block);
        b.store(b.slot_addr(slot), b.const_int(mir::Ty::I64, 1));
        b.jump(join);
        b.set_insert_point(join);
        b.ret(b.load(mir::Ty::I64, b.slot_addr(slot)));

        const auto stats = mir::promote_slots(f.module);
        const auto errors = mir::verify(f.module);
        check(errors.empty(), "edge-split promotion verifies" + describe(errors));
        check(stats.slots_promoted == 1 && stats.params_added == 1,
              "the join takes the value as a parameter");
        const auto text = mir::print(f.module);
        check(text.find(".edge") != std::string::npos,
              "the branch edge into the parameterized join is split through a trampoline");
    }

    void test_promote_slots_leaves_escaping_and_mixed_slots() {
        Fixture f;
        mir::Builder b(f.module, f.fn_index);
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);
        const auto arg = b.add_block_param(entry, mir::Ty::I64);
        // Slot 0 escapes: its address is passed to a call... simulated by marking it,
        // exactly as mirgen does for '&x'.
        const auto escaping = b.add_slot(8, 8, "escapes");
        b.mark_slot_escaping(escaping);
        b.store(b.slot_addr(escaping), arg);
        // Slot 1 is accessed at two widths -- an aggregate reinterpret promote_slots
        // must refuse.
        const auto mixed = b.add_slot(8, 8, "mixed");
        b.store(b.slot_addr(mixed), arg);
        const auto narrow = b.load(mir::Ty::I32, b.slot_addr(mixed));
        b.ret(b.convert(mir::Op::ZExt, mir::Ty::I64, narrow));

        const auto stats = mir::promote_slots(f.module);
        const auto errors = mir::verify(f.module);
        check(errors.empty(), "refusal cases verify" + describe(errors));
        check(stats.slots_promoted == 0, "neither slot is promoted");
        check(f.module.functions[0].slots.size() == 2, "both stay in the frame");
    }

    // ---- peephole (stage 3) -------------------------------------------------------

    void test_peephole_folds_and_cleans() {
        Fixture f;
        mir::Builder b(f.module, f.fn_index);
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);
        const auto arg = b.add_block_param(entry, mir::Ty::I64);
        // (40 + 2) computed from constants; x + 0 an identity; a dead multiply.
        const auto forty = b.const_int(mir::Ty::I64, 40);
        const auto two = b.const_int(mir::Ty::I64, 2);
        const auto sum = b.binary(mir::Op::Add, mir::Ty::I64, forty, two);
        const auto zero = b.const_int(mir::Ty::I64, 0);
        const auto same = b.binary(mir::Op::Add, mir::Ty::I64, arg, zero);
        (void) b.binary(mir::Op::Mul, mir::Ty::I64, arg, sum); // never used
        b.ret(b.binary(mir::Op::Add, mir::Ty::I64, same, sum));

        const auto stats = mir::peephole(f.module);
        const auto errors = mir::verify(f.module);
        check(errors.empty(), "peephole output verifies" + describe(errors));
        check(stats.folded >= 1, "constants fold");
        check(stats.simplified >= 1, "x + 0 simplifies to x");
        check(stats.dead_removed >= 1, "the unused multiply is removed");
        const auto text = mir::print(f.module);
        check(text.find("const.int 42") != std::string::npos, "40 + 2 becomes 42");
        check(text.find("mul") == std::string::npos, "no dead multiply remains");
    }

    void test_peephole_keeps_division_by_zero() {
        // Folding '1 / 0' away would delete runtime behavior; the instruction stays.
        Fixture f;
        mir::Builder b(f.module, f.fn_index);
        const auto entry = b.create_block("entry");
        b.set_insert_point(entry);
        (void) b.add_block_param(entry, mir::Ty::I64);
        const auto one = b.const_int(mir::Ty::I64, 1);
        const auto zero = b.const_int(mir::Ty::I64, 0);
        b.ret(b.binary(mir::Op::UDiv, mir::Ty::I64, one, zero));

        (void) mir::peephole(f.module);
        const auto errors = mir::verify(f.module);
        check(errors.empty(), "division by zero survives verification" + describe(errors));
        check(mir::print(f.module).find("udiv") != std::string::npos,
              "and is not folded away");
    }

    void test_peephole_removes_trivial_params() {
        // Both arms pass the SAME value to the join, so its parameter is redundant --
        // exactly the shape promote_slots deliberately leaves for this pass.
        Fixture f(mir::Ty::I64, {mir::Ty::I1, mir::Ty::I64});
        mir::Builder b(f.module, f.fn_index);
        const auto entry = b.create_block("entry");
        const auto left = b.create_block("left");
        const auto right = b.create_block("right");
        const auto join = b.create_block("join");
        b.set_insert_point(entry);
        const auto cond = b.add_block_param(entry, mir::Ty::I1);
        const auto x = b.add_block_param(entry, mir::Ty::I64);
        b.branch(cond, left, right);
        const auto param = b.add_block_param(join, mir::Ty::I64);
        b.set_insert_point(left);
        b.jump(join, {x});
        b.set_insert_point(right);
        b.jump(join, {x});
        b.set_insert_point(join);
        b.ret(b.binary(mir::Op::Add, mir::Ty::I64, param, param));

        const auto stats = mir::peephole(f.module);
        const auto errors = mir::verify(f.module);
        check(errors.empty(), "trivial-param removal verifies" + describe(errors));
        check(stats.params_removed == 1, "the redundant parameter is removed");
        check(f.module.functions[0].blocks[3].params.empty(), "the join takes no parameters");
    }

    void test_printer_renders_globals_and_relocations() {
        mir::Module module;
        module.name = "test";
        const auto sig = module.intern_signature(mir::Signature{.result = mir::Ty::Void});
        module.functions.push_back(mir::Function{.name = "target", .signature = sig, .has_body = false});
        module.globals.push_back(mir::Global{
            .name = "vtable",
            .linkage = mir::Linkage::Internal,
            .size = 8,
            .align = 8,
            .is_constant = true,
            .init = std::vector<uint8_t>(8, 0),
            .relocations = {mir::Relocation{.kind = mir::Relocation::Kind::FunctionAddr, .offset = 0, .target = 0}},
        });
        module.globals.push_back(mir::Global{.name = "counter", .size = 4, .align = 4});

        const auto text = mir::print(module);
        check(text.find("const @vtable: size 8, align 8") != std::string::npos, "a constant global is rendered");
        check(text.find("+0 -> @target") != std::string::npos, "its relocation is rendered");
        check(text.find("global @counter") != std::string::npos, "a mutable global is rendered");
        check(text.find("zeroinit") != std::string::npos, "an uninitialized global is marked zeroinit");
    }
}

int main() {
    test_builds_and_verifies_a_simple_function();
    test_block_params_replace_phi();
    test_slots_and_memory();
    test_ptr_add_const_folds_zero();
    test_signature_interning();

    test_rejects_block_without_terminator();
    test_rejects_terminator_in_the_middle();
    test_rejects_empty_block();
    test_rejects_out_of_range_branch_target();
    test_rejects_block_argument_mismatch();
    test_rejects_type_mismatched_operands();
    test_rejects_wrong_return_shape();
    test_rejects_non_pointer_load();
    test_rejects_non_i1_branch_condition();
    test_rejects_call_arity_and_type_errors();
    test_rejects_dangling_references();
    test_rejects_identity_conversion();
    test_rejects_declaration_with_blocks();
    test_promote_slots_straight_line();
    test_promote_slots_diamond_becomes_block_param();
    test_promote_slots_loop_counter();
    test_promote_slots_splits_branch_edges();
    test_promote_slots_leaves_escaping_and_mixed_slots();
    test_peephole_folds_and_cleans();
    test_peephole_keeps_division_by_zero();
    test_peephole_removes_trivial_params();
    test_printer_renders_globals_and_relocations();

    if (failures != 0) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("\nall MIR tests passed\n");
    return 0;
}

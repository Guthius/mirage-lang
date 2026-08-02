#include "mir.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace mir {
    auto type_name(const Ty type) -> const char * {
        switch (type) {
        case Ty::Void: return "void";
        case Ty::I1:   return "i1";
        case Ty::I8:   return "i8";
        case Ty::I16:  return "i16";
        case Ty::I32:  return "i32";
        case Ty::I64:  return "i64";
        case Ty::F32:  return "f32";
        case Ty::F64:  return "f64";
        case Ty::Ptr:  return "ptr";
        }
        return "?";
    }

    auto type_bits(const Ty type, const uint32_t pointer_bits) -> uint32_t {
        switch (type) {
        case Ty::Void: return 0;
        case Ty::I1:   return 1;
        case Ty::I8:   return 8;
        case Ty::I16:  return 16;
        case Ty::I32:  return 32;
        case Ty::I64:  return 64;
        case Ty::F32:  return 32;
        case Ty::F64:  return 64;
        case Ty::Ptr:  return pointer_bits;
        }
        return 0;
    }

    auto is_float(const Ty type) -> bool { return type == Ty::F32 || type == Ty::F64; }

    auto is_integer(const Ty type) -> bool {
        return type == Ty::I1 || type == Ty::I8 || type == Ty::I16 || type == Ty::I32 || type == Ty::I64;
    }

    auto op_name(const Op op) -> const char * {
        switch (op) {
        case Op::ConstInt:     return "const.int";
        case Op::ConstFloat:   return "const.float";
        case Op::ConstNull:    return "const.null";
        case Op::GlobalAddr:   return "global.addr";
        case Op::SlotAddr:     return "slot.addr";
        case Op::FuncAddr:     return "func.addr";
        case Op::Load:         return "load";
        case Op::Store:        return "store";
        case Op::MemCopy:      return "mem.copy";
        case Op::MemSet:       return "mem.set";
        case Op::PtrAddConst:  return "ptr.add.const";
        case Op::PtrAdd:       return "ptr.add";
        case Op::Add:          return "add";
        case Op::Sub:          return "sub";
        case Op::Mul:          return "mul";
        case Op::SDiv:         return "sdiv";
        case Op::UDiv:         return "udiv";
        case Op::SRem:         return "srem";
        case Op::URem:         return "urem";
        case Op::And:          return "and";
        case Op::Or:           return "or";
        case Op::Xor:          return "xor";
        case Op::Shl:          return "shl";
        case Op::LShr:         return "lshr";
        case Op::AShr:         return "ashr";
        case Op::Not:          return "not";
        case Op::Neg:          return "neg";
        case Op::FAdd:         return "fadd";
        case Op::FSub:         return "fsub";
        case Op::FMul:         return "fmul";
        case Op::FDiv:         return "fdiv";
        case Op::FRem:         return "frem";
        case Op::FNeg:         return "fneg";
        case Op::ICmpEq:       return "icmp.eq";
        case Op::ICmpNe:       return "icmp.ne";
        case Op::ICmpSlt:      return "icmp.slt";
        case Op::ICmpSle:      return "icmp.sle";
        case Op::ICmpSgt:      return "icmp.sgt";
        case Op::ICmpSge:      return "icmp.sge";
        case Op::ICmpUlt:      return "icmp.ult";
        case Op::ICmpUle:      return "icmp.ule";
        case Op::ICmpUgt:      return "icmp.ugt";
        case Op::ICmpUge:      return "icmp.uge";
        case Op::FCmpOeq:      return "fcmp.oeq";
        case Op::FCmpOne:      return "fcmp.one";
        case Op::FCmpOlt:      return "fcmp.olt";
        case Op::FCmpOle:      return "fcmp.ole";
        case Op::FCmpOgt:      return "fcmp.ogt";
        case Op::FCmpOge:      return "fcmp.oge";
        case Op::Trunc:        return "trunc";
        case Op::ZExt:         return "zext";
        case Op::SExt:         return "sext";
        case Op::FPTrunc:      return "fptrunc";
        case Op::FPExt:        return "fpext";
        case Op::FPToSI:       return "fptosi";
        case Op::FPToUI:       return "fptoui";
        case Op::SIToFP:       return "sitofp";
        case Op::UIToFP:       return "uitofp";
        case Op::PtrToInt:     return "ptrtoint";
        case Op::IntToPtr:     return "inttoptr";
        case Op::Bitcast:      return "bitcast";
        case Op::Select:       return "select";
        case Op::Call:         return "call";
        case Op::CallIndirect: return "call.indirect";
        case Op::Jump:         return "jump";
        case Op::Branch:       return "branch";
        case Op::Switch:       return "switch";
        case Op::Return:       return "return";
        case Op::Unreachable:  return "unreachable";
        }
        return "?";
    }

    auto is_terminator(const Op op) -> bool {
        return op == Op::Jump || op == Op::Branch || op == Op::Switch ||
               op == Op::Return || op == Op::Unreachable;
    }

    auto defines_value(const Op op) -> bool {
        if (is_terminator(op)) return false;
        return op != Op::Store && op != Op::MemCopy && op != Op::MemSet;
    }

    namespace {
        auto is_comparison(const Op op) -> bool {
            return (op >= Op::ICmpEq && op <= Op::ICmpUge) || (op >= Op::FCmpOeq && op <= Op::FCmpOge);
        }

        auto is_float_comparison(const Op op) -> bool {
            return op >= Op::FCmpOeq && op <= Op::FCmpOge;
        }

        auto is_int_binary(const Op op) -> bool {
            return op >= Op::Add && op <= Op::AShr;
        }
    }

    auto Module::intern_signature(Signature sig) -> uint32_t {
        for (size_t i = 0; i < signatures.size(); ++i) {
            const auto &existing = signatures[i];
            if (existing.result == sig.result && existing.is_variadic == sig.is_variadic &&
                existing.params == sig.params) {
                return static_cast<uint32_t>(i);
            }
        }
        signatures.push_back(std::move(sig));
        return static_cast<uint32_t>(signatures.size() - 1);
    }

    // ---------------------------------------------------------------- Builder

    Builder::Builder(Module &module, const uint32_t function_index)
        : module_(module), function_(function_index) {}

    auto Builder::create_block(std::string label) -> BlockId {
        auto &fn = function();
        fn.blocks.push_back(Block{.label = std::move(label)});
        return static_cast<BlockId>(fn.blocks.size() - 1);
    }

    auto Builder::new_value(const Ty type, const BlockId block, const uint32_t index, const bool is_param) -> ValueId {
        auto &fn = function();
        fn.values.push_back(ValueDef{.block = block, .index = index, .type = type, .is_param = is_param});
        return static_cast<ValueId>(fn.values.size() - 1);
    }

    auto Builder::add_block_param(const BlockId block, const Ty type) -> ValueId {
        auto &fn = function();
        const auto index = static_cast<uint32_t>(fn.blocks[block].params.size());
        const auto id = new_value(type, block, index, /*is_param=*/true);
        fn.blocks[block].params.push_back(id);
        return id;
    }

    void Builder::set_insert_point(const BlockId block) { current_ = block; }

    auto Builder::block_is_terminated() const -> bool {
        if (current_ == NO_BLOCK) return true;
        const auto &insts = function().blocks[current_].insts;
        return !insts.empty() && is_terminator(insts.back().op);
    }

    auto Builder::add_slot(const uint32_t size, const uint32_t align, std::string name) -> uint32_t {
        auto &fn = function();
        fn.slots.push_back(Slot{.size = size, .align = std::max(1u, align), .name = std::move(name)});
        return static_cast<uint32_t>(fn.slots.size() - 1);
    }

    void Builder::mark_slot_escaping(const uint32_t slot) {
        function().slots[slot].address_escapes = true;
    }

    void Builder::set_location(const uint32_t line, const uint32_t column) {
        line_ = line;
        column_ = column;
    }

    auto Builder::emit(Inst inst) -> ValueId {
        auto &fn = function();
        inst.line = line_;
        inst.column = column_;

        auto &block = fn.blocks[current_];
        const auto index = static_cast<uint32_t>(block.insts.size());

        ValueId result = NO_VALUE;
        if (defines_value(inst.op) && inst.type != Ty::Void) {
            result = new_value(inst.type, current_, index, /*is_param=*/false);
            inst.result = result;
        }
        block.insts.push_back(std::move(inst));
        return result;
    }

    auto Builder::const_int(const Ty type, const int64_t value) -> ValueId {
        return emit(Inst{.op = Op::ConstInt, .type = type, .imm = value});
    }

    auto Builder::const_float(const Ty type, const double value) -> ValueId {
        // Stored as a bit pattern so the printer and the backends agree exactly; going
        // through a decimal rendering would not round-trip.
        int64_t bits = 0;
        if (type == Ty::F32) {
            const auto f = static_cast<float>(value);
            uint32_t raw = 0;
            std::memcpy(&raw, &f, sizeof raw);
            bits = raw;
        } else {
            std::memcpy(&bits, &value, sizeof bits);
        }
        return emit(Inst{.op = Op::ConstFloat, .type = type, .imm = bits});
    }

    auto Builder::const_null() -> ValueId {
        return emit(Inst{.op = Op::ConstNull, .type = Ty::Ptr});
    }

    auto Builder::global_addr(const uint32_t global_index) -> ValueId {
        return emit(Inst{.op = Op::GlobalAddr, .type = Ty::Ptr, .a = global_index});
    }

    auto Builder::slot_addr(const uint32_t slot_index) -> ValueId {
        return emit(Inst{.op = Op::SlotAddr, .type = Ty::Ptr, .a = slot_index});
    }

    auto Builder::func_addr(const uint32_t function_index) -> ValueId {
        return emit(Inst{.op = Op::FuncAddr, .type = Ty::Ptr, .a = function_index});
    }

    auto Builder::load(const Ty type, const ValueId address) -> ValueId {
        return emit(Inst{.op = Op::Load, .type = type, .a = address});
    }

    void Builder::store(const ValueId address, const ValueId value) {
        emit(Inst{.op = Op::Store, .type = Ty::Void, .a = address, .b = value});
    }

    void Builder::mem_copy(const ValueId dst, const ValueId src, const ValueId bytes) {
        emit(Inst{.op = Op::MemCopy, .type = Ty::Void, .a = dst, .b = src, .c = bytes});
    }

    void Builder::mem_set(const ValueId dst, const ValueId byte, const ValueId bytes) {
        emit(Inst{.op = Op::MemSet, .type = Ty::Void, .a = dst, .b = byte, .c = bytes});
    }

    auto Builder::ptr_add_const(const ValueId base, const int64_t byte_offset) -> ValueId {
        // Folding a zero offset here rather than in a later pass keeps the emitted MIR
        // readable: struct field 0 is by far the most common access and would otherwise
        // produce a no-op instruction at every one.
        if (byte_offset == 0) {
            return base;
        }
        return emit(Inst{.op = Op::PtrAddConst, .type = Ty::Ptr, .a = base, .imm = byte_offset});
    }

    auto Builder::ptr_add(const ValueId base, const ValueId byte_offset) -> ValueId {
        return emit(Inst{.op = Op::PtrAdd, .type = Ty::Ptr, .a = base, .b = byte_offset});
    }

    auto Builder::binary(const Op op, const Ty type, const ValueId lhs, const ValueId rhs) -> ValueId {
        return emit(Inst{.op = op, .type = type, .a = lhs, .b = rhs});
    }

    auto Builder::unary(const Op op, const Ty type, const ValueId operand) -> ValueId {
        return emit(Inst{.op = op, .type = type, .a = operand});
    }

    auto Builder::compare(const Op op, const ValueId lhs, const ValueId rhs) -> ValueId {
        return emit(Inst{.op = op, .type = Ty::I1, .a = lhs, .b = rhs});
    }

    auto Builder::convert(const Op op, const Ty type, const ValueId operand) -> ValueId {
        return emit(Inst{.op = op, .type = type, .a = operand});
    }

    auto Builder::select(const ValueId condition, const ValueId if_true, const ValueId if_false, const Ty type) -> ValueId {
        return emit(Inst{.op = Op::Select, .type = type, .a = condition, .b = if_true, .c = if_false});
    }

    auto Builder::call(const uint32_t callee, const Ty result_type, const std::vector<ValueId> &args) -> ValueId {
        return emit(Inst{.op = Op::Call, .type = result_type, .a = callee, .args = args});
    }

    auto Builder::call_indirect(const ValueId callee, const uint32_t signature, const Ty result_type,
                                 const std::vector<ValueId> &args) -> ValueId {
        return emit(Inst{.op = Op::CallIndirect, .type = result_type, .a = callee, .b = signature, .args = args});
    }

    void Builder::jump(const BlockId target, const std::vector<ValueId> &args) {
        emit(Inst{.op = Op::Jump, .type = Ty::Void, .a = target, .args = args});
    }

    void Builder::branch(const ValueId condition, const BlockId if_true, const BlockId if_false) {
        emit(Inst{.op = Op::Branch, .type = Ty::Void, .a = condition, .b = if_true, .c = if_false});
    }

    void Builder::switch_on(const ValueId scrutinee, const BlockId default_block,
                             const std::vector<std::pair<int64_t, BlockId>> &cases) {
        Inst inst{.op = Op::Switch, .type = Ty::Void, .a = scrutinee, .b = default_block};
        // Flattened (value, block) pairs: the case values are 32-bit-truncated here because
        // every switch the front end emits is on a tag or a small enum. The verifier checks
        // the pairing, and widening this later is a local change.
        for (const auto &[value, block] : cases) {
            inst.args.push_back(static_cast<uint32_t>(value));
            inst.args.push_back(block);
        }
        emit(std::move(inst));
    }

    void Builder::ret(const ValueId value) {
        Inst inst{.op = Op::Return, .type = Ty::Void};
        if (value != NO_VALUE) {
            inst.args.push_back(value);
        }
        emit(std::move(inst));
    }

    void Builder::unreachable() {
        emit(Inst{.op = Op::Unreachable, .type = Ty::Void});
    }

    auto Builder::value_type(const ValueId value) const -> Ty {
        const auto &fn = function();
        return value < fn.values.size() ? fn.values[value].type : Ty::Void;
    }

    // ---------------------------------------------------------------- verify

    namespace {
        struct Verifier {
            const Module &module;
            uint32_t function_index = 0;
            std::vector<VerifyError> errors;

            void fail(std::string message) {
                errors.push_back(VerifyError{.function = function_index, .message = std::move(message)});
            }

            [[nodiscard]] auto value_ok(const Function &fn, const uint32_t value) const -> bool {
                return value != NO_VALUE && value < fn.values.size();
            }

            void check_operand(const Function &fn, const uint32_t value, const std::string &where) {
                if (!value_ok(fn, value)) {
                    fail(std::format("{}: operand {} is not a value defined in this function", where, value));
                }
            }

            void check_operand_type(const Function &fn, const uint32_t value, const Ty expected, const std::string &where) {
                if (!value_ok(fn, value)) {
                    fail(std::format("{}: operand {} is not a value defined in this function", where, value));
                    return;
                }
                if (fn.values[value].type != expected) {
                    fail(std::format("{}: operand has type '{}', expected '{}'",
                                     where, type_name(fn.values[value].type), type_name(expected)));
                }
            }

            void check_block_args(const Function &fn, const BlockId target, const std::vector<uint32_t> &args,
                                   const std::string &where) {
                if (target >= fn.blocks.size()) {
                    fail(std::format("{}: branch target {} is out of range", where, target));
                    return;
                }
                const auto &params = fn.blocks[target].params;
                if (params.size() != args.size()) {
                    fail(std::format("{}: target block '{}' takes {} parameter(s) but {} argument(s) were passed",
                                     where, fn.blocks[target].label, params.size(), args.size()));
                    return;
                }
                for (size_t i = 0; i < args.size(); ++i) {
                    if (!value_ok(fn, args[i])) {
                        fail(std::format("{}: block argument {} is not a value defined in this function", where, i));
                        continue;
                    }
                    if (fn.values[args[i]].type != fn.values[params[i]].type) {
                        fail(std::format("{}: block argument {} has type '{}' but parameter is '{}'",
                                         where, i, type_name(fn.values[args[i]].type), type_name(fn.values[params[i]].type)));
                    }
                }
            }

            void check_function(const Function &fn) {
                if (!fn.has_body) {
                    if (!fn.blocks.empty()) {
                        fail(std::format("function '{}' is a declaration but has blocks", fn.name));
                    }
                    return;
                }
                if (fn.blocks.empty()) {
                    fail(std::format("function '{}' has a body but no blocks", fn.name));
                    return;
                }
                if (fn.signature >= module.signatures.size()) {
                    fail(std::format("function '{}' references signature {}, which does not exist", fn.name, fn.signature));
                    return;
                }
                const auto &sig = module.signatures[fn.signature];

                for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
                    const auto &block = fn.blocks[bi];
                    const auto where = std::format("{}:{}", fn.name, block.label.empty() ? std::format("block{}", bi) : block.label);

                    if (block.insts.empty()) {
                        fail(std::format("{}: block has no instructions (every block must end in a terminator)", where));
                        continue;
                    }
                    // Exactly one terminator, at the end. A terminator in the middle means
                    // the emitter kept writing into a closed block, which is the single most
                    // common shape of emitter bug.
                    for (size_t ii = 0; ii + 1 < block.insts.size(); ++ii) {
                        if (is_terminator(block.insts[ii].op)) {
                            fail(std::format("{}: '{}' at instruction {} terminates the block but is not last",
                                             where, op_name(block.insts[ii].op), ii));
                        }
                    }
                    if (!is_terminator(block.insts.back().op)) {
                        fail(std::format("{}: block does not end in a terminator (ends in '{}')",
                                         where, op_name(block.insts.back().op)));
                    }

                    for (const auto &inst : block.insts) {
                        check_inst(fn, sig, inst, where);
                    }
                }
            }

            void check_inst(const Function &fn, const Signature &sig, const Inst &inst, const std::string &where) {
                const auto site = std::format("{}: '{}'", where, op_name(inst.op));

                switch (inst.op) {
                case Op::ConstInt:
                    if (!is_integer(inst.type)) fail(std::format("{}: result must be an integer type", site));
                    break;
                case Op::ConstFloat:
                    if (!is_float(inst.type)) fail(std::format("{}: result must be a float type", site));
                    break;
                case Op::ConstNull:
                case Op::SlotAddr:
                case Op::GlobalAddr:
                case Op::FuncAddr:
                    if (inst.type != Ty::Ptr) fail(std::format("{}: result must be 'ptr'", site));
                    if (inst.op == Op::SlotAddr && inst.a >= fn.slots.size()) {
                        fail(std::format("{}: slot {} does not exist", site, inst.a));
                    }
                    if (inst.op == Op::GlobalAddr && inst.a >= module.globals.size()) {
                        fail(std::format("{}: global {} does not exist", site, inst.a));
                    }
                    if (inst.op == Op::FuncAddr && inst.a >= module.functions.size()) {
                        fail(std::format("{}: function {} does not exist", site, inst.a));
                    }
                    break;

                case Op::Load:
                    check_operand_type(fn, inst.a, Ty::Ptr, site);
                    if (inst.type == Ty::Void) fail(std::format("{}: cannot load a void value", site));
                    break;
                case Op::Store:
                    check_operand_type(fn, inst.a, Ty::Ptr, site);
                    check_operand(fn, inst.b, site);
                    break;
                case Op::MemCopy:
                    check_operand_type(fn, inst.a, Ty::Ptr, site);
                    check_operand_type(fn, inst.b, Ty::Ptr, site);
                    check_operand(fn, inst.c, site);
                    break;
                case Op::MemSet:
                    check_operand_type(fn, inst.a, Ty::Ptr, site);
                    check_operand(fn, inst.b, site);
                    check_operand(fn, inst.c, site);
                    break;

                case Op::PtrAddConst:
                    check_operand_type(fn, inst.a, Ty::Ptr, site);
                    if (inst.type != Ty::Ptr) fail(std::format("{}: result must be 'ptr'", site));
                    break;
                case Op::PtrAdd:
                    check_operand_type(fn, inst.a, Ty::Ptr, site);
                    check_operand(fn, inst.b, site);
                    if (inst.type != Ty::Ptr) fail(std::format("{}: result must be 'ptr'", site));
                    break;

                case Op::Select:
                    check_operand_type(fn, inst.a, Ty::I1, site);
                    check_operand_type(fn, inst.b, inst.type, site);
                    check_operand_type(fn, inst.c, inst.type, site);
                    break;

                case Op::Call: {
                    if (inst.a >= module.functions.size()) {
                        fail(std::format("{}: callee {} does not exist", site, inst.a));
                        break;
                    }
                    const auto &callee = module.functions[inst.a];
                    if (callee.signature >= module.signatures.size()) break;
                    const auto &callee_sig = module.signatures[callee.signature];
                    if (!callee_sig.is_variadic && inst.args.size() != callee_sig.params.size()) {
                        fail(std::format("{}: '{}' takes {} argument(s), {} passed",
                                         site, callee.name, callee_sig.params.size(), inst.args.size()));
                    }
                    for (size_t i = 0; i < inst.args.size(); ++i) {
                        if (i < callee_sig.params.size()) {
                            check_operand_type(fn, inst.args[i], callee_sig.params[i], site);
                        } else {
                            check_operand(fn, inst.args[i], site);
                        }
                    }
                    if (inst.type != callee_sig.result) {
                        fail(std::format("{}: '{}' returns '{}' but the call is typed '{}'",
                                         site, callee.name, type_name(callee_sig.result), type_name(inst.type)));
                    }
                    break;
                }
                case Op::CallIndirect: {
                    check_operand_type(fn, inst.a, Ty::Ptr, site);
                    if (inst.b >= module.signatures.size()) {
                        fail(std::format("{}: signature {} does not exist", site, inst.b));
                        break;
                    }
                    const auto &target_sig = module.signatures[inst.b];
                    if (!target_sig.is_variadic && inst.args.size() != target_sig.params.size()) {
                        fail(std::format("{}: signature takes {} argument(s), {} passed",
                                         site, target_sig.params.size(), inst.args.size()));
                    }
                    for (size_t i = 0; i < inst.args.size() && i < target_sig.params.size(); ++i) {
                        check_operand_type(fn, inst.args[i], target_sig.params[i], site);
                    }
                    break;
                }

                case Op::Jump:
                    check_block_args(fn, inst.a, inst.args, site);
                    break;
                case Op::Branch:
                    check_operand_type(fn, inst.a, Ty::I1, site);
                    // A conditional branch passes no block arguments; a target that takes
                    // parameters needs an intervening block. Checked so the omission is
                    // caught here rather than as wrong values at runtime.
                    check_block_args(fn, inst.b, {}, site);
                    check_block_args(fn, inst.c, {}, site);
                    break;
                case Op::Switch: {
                    check_operand(fn, inst.a, site);
                    check_block_args(fn, inst.b, {}, site);
                    if (inst.args.size() % 2 != 0) {
                        fail(std::format("{}: case list is not (value, block) pairs", site));
                        break;
                    }
                    for (size_t i = 1; i < inst.args.size(); i += 2) {
                        check_block_args(fn, inst.args[i], {}, site);
                    }
                    break;
                }
                case Op::Return:
                    if (sig.result == Ty::Void) {
                        if (!inst.args.empty()) {
                            fail(std::format("{}: function returns void but a value was returned", site));
                        }
                    } else if (inst.args.size() != 1) {
                        fail(std::format("{}: function returns '{}' but {} value(s) were returned",
                                         site, type_name(sig.result), inst.args.size()));
                    } else {
                        check_operand_type(fn, inst.args[0], sig.result, site);
                    }
                    break;
                case Op::Unreachable:
                    break;

                default:
                    // Arithmetic, comparison and conversion.
                    if (is_comparison(inst.op)) {
                        if (inst.type != Ty::I1) {
                            fail(std::format("{}: a comparison must produce 'i1'", site));
                        }
                        check_operand(fn, inst.a, site);
                        check_operand(fn, inst.b, site);
                        if (value_ok(fn, inst.a) && value_ok(fn, inst.b)) {
                            const auto lhs = fn.values[inst.a].type;
                            const auto rhs = fn.values[inst.b].type;
                            if (lhs != rhs) {
                                fail(std::format("{}: operands have different types ('{}' and '{}')",
                                                 site, type_name(lhs), type_name(rhs)));
                            } else if (is_float_comparison(inst.op) != is_float(lhs) && lhs != Ty::Ptr) {
                                fail(std::format("{}: operand type '{}' does not match the comparison kind",
                                                 site, type_name(lhs)));
                            }
                        }
                    } else if (is_int_binary(inst.op) && inst.op != Op::Not && inst.op != Op::Neg) {
                        check_operand_type(fn, inst.a, inst.type, site);
                        check_operand_type(fn, inst.b, inst.type, site);
                        if (!is_integer(inst.type)) {
                            fail(std::format("{}: integer operation on '{}'", site, type_name(inst.type)));
                        }
                    } else if (inst.op == Op::Not || inst.op == Op::Neg || inst.op == Op::FNeg) {
                        check_operand_type(fn, inst.a, inst.type, site);
                    } else if (inst.op >= Op::FAdd && inst.op <= Op::FRem) {
                        check_operand_type(fn, inst.a, inst.type, site);
                        check_operand_type(fn, inst.b, inst.type, site);
                        if (!is_float(inst.type)) {
                            fail(std::format("{}: float operation on '{}'", site, type_name(inst.type)));
                        }
                    } else {
                        // Conversions: one operand, and the result must genuinely differ from
                        // it -- a same-type "conversion" is a sign the emitter lost track.
                        check_operand(fn, inst.a, site);
                        if (value_ok(fn, inst.a) && fn.values[inst.a].type == inst.type && inst.op != Op::Bitcast) {
                            fail(std::format("{}: converts '{}' to itself", site, type_name(inst.type)));
                        }
                    }
                    break;
                }
            }
        };
    }

    auto verify(const Module &module) -> std::vector<VerifyError> {
        Verifier verifier{.module = module};
        for (size_t i = 0; i < module.functions.size(); ++i) {
            verifier.function_index = static_cast<uint32_t>(i);
            verifier.check_function(module.functions[i]);
        }
        return std::move(verifier.errors);
    }

    // ---------------------------------------------------------------- print

    namespace {
        auto value_ref(const Function &fn, const uint32_t value) -> std::string {
            if (value == NO_VALUE) return "<none>";
            if (value >= fn.values.size()) return std::format("<bad {}>", value);
            return std::format("%{}", value);
        }

        auto block_ref(const Function &fn, const uint32_t block) -> std::string {
            if (block >= fn.blocks.size()) return std::format("<bad block {}>", block);
            const auto &label = fn.blocks[block].label;
            return label.empty() ? std::format("^{}", block) : std::format("^{}", label);
        }

        void print_inst(std::string &out, const Module &module, const Function &fn, const Inst &inst) {
            out += "    ";
            if (inst.result != NO_VALUE) {
                out += std::format("{}: {} = ", value_ref(fn, inst.result), type_name(inst.type));
            }
            out += op_name(inst.op);

            switch (inst.op) {
            case Op::ConstInt:
                out += std::format(" {}", inst.imm);
                break;
            case Op::ConstFloat:
                // The bit pattern, not a decimal rendering: this has to round-trip exactly,
                // and it is what the backends will encode.
                out += std::format(" 0x{:016x}", static_cast<uint64_t>(inst.imm));
                break;
            case Op::ConstNull:
            case Op::Unreachable:
                break;
            case Op::SlotAddr:
                out += std::format(" @slot{}", inst.a);
                if (inst.a < fn.slots.size() && !fn.slots[inst.a].name.empty()) {
                    out += std::format(" ; {}", fn.slots[inst.a].name);
                }
                break;
            case Op::GlobalAddr:
                out += inst.a < module.globals.size()
                    ? std::format(" @{}", module.globals[inst.a].name)
                    : std::format(" @<bad {}>", inst.a);
                break;
            case Op::FuncAddr:
                out += inst.a < module.functions.size()
                    ? std::format(" @{}", module.functions[inst.a].name)
                    : std::format(" @<bad {}>", inst.a);
                break;
            case Op::PtrAddConst:
                out += std::format(" {}, {}", value_ref(fn, inst.a), inst.imm);
                break;
            case Op::Call: {
                out += inst.a < module.functions.size()
                    ? std::format(" @{}(", module.functions[inst.a].name)
                    : std::format(" @<bad {}>(", inst.a);
                for (size_t i = 0; i < inst.args.size(); ++i) {
                    if (i) out += ", ";
                    out += value_ref(fn, inst.args[i]);
                }
                out += ")";
                break;
            }
            case Op::CallIndirect: {
                out += std::format(" {} : sig{}(", value_ref(fn, inst.a), inst.b);
                for (size_t i = 0; i < inst.args.size(); ++i) {
                    if (i) out += ", ";
                    out += value_ref(fn, inst.args[i]);
                }
                out += ")";
                break;
            }
            case Op::Jump: {
                out += std::format(" {}", block_ref(fn, inst.a));
                if (!inst.args.empty()) {
                    out += "(";
                    for (size_t i = 0; i < inst.args.size(); ++i) {
                        if (i) out += ", ";
                        out += value_ref(fn, inst.args[i]);
                    }
                    out += ")";
                }
                break;
            }
            case Op::Branch:
                out += std::format(" {}, {}, {}", value_ref(fn, inst.a), block_ref(fn, inst.b), block_ref(fn, inst.c));
                break;
            case Op::Switch: {
                out += std::format(" {}, default {}", value_ref(fn, inst.a), block_ref(fn, inst.b));
                for (size_t i = 0; i + 1 < inst.args.size(); i += 2) {
                    out += std::format(", {} -> {}", static_cast<int32_t>(inst.args[i]), block_ref(fn, inst.args[i + 1]));
                }
                break;
            }
            case Op::Return:
                for (size_t i = 0; i < inst.args.size(); ++i) {
                    out += std::format("{}{}", i ? ", " : " ", value_ref(fn, inst.args[i]));
                }
                break;
            default: {
                const uint32_t operands[] = {inst.a, inst.b, inst.c};
                bool first = true;
                for (const auto operand : operands) {
                    if (operand == NO_VALUE) continue;
                    out += std::format("{}{}", first ? " " : ", ", value_ref(fn, operand));
                    first = false;
                }
                break;
            }
            }
            out += "\n";
        }
    }

    auto print(const Module &module, const Function &fn) -> std::string {
        std::string out;

        const auto &sig = fn.signature < module.signatures.size() ? module.signatures[fn.signature] : Signature{};
        out += fn.has_body ? "fn " : "declare ";
        if (fn.linkage == Linkage::External) out += "export ";
        if (fn.conv == CallConv::C) out += "cdecl ";
        out += std::format("@{}(", fn.name);
        for (size_t i = 0; i < sig.params.size(); ++i) {
            if (i) out += ", ";
            out += i < fn.params.size()
                ? std::format("{}: {}", value_ref(fn, fn.params[i]), type_name(sig.params[i]))
                : std::string(type_name(sig.params[i]));
        }
        if (sig.is_variadic) out += sig.params.empty() ? "..." : ", ...";
        out += ")";
        if (sig.result != Ty::Void) out += std::format(" -> {}", type_name(sig.result));
        if (!fn.import_module.empty()) {
            out += std::format(" import(\"{}\", \"{}\")", fn.import_module, fn.import_name);
        }
        if (!fn.section.empty()) out += std::format(" section(\"{}\")", fn.section);

        if (!fn.has_body) {
            out += "\n";
            return out;
        }
        out += " {\n";

        for (size_t i = 0; i < fn.slots.size(); ++i) {
            const auto &slot = fn.slots[i];
            out += std::format("  slot{}: size {}, align {}{}{}\n", i, slot.size, slot.align,
                                slot.address_escapes ? ", escapes" : "",
                                slot.name.empty() ? "" : std::format(" ; {}", slot.name));
        }

        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            const auto &block = fn.blocks[bi];
            out += std::format("  {}", block_ref(fn, static_cast<uint32_t>(bi)));
            if (!block.params.empty()) {
                out += "(";
                for (size_t i = 0; i < block.params.size(); ++i) {
                    if (i) out += ", ";
                    out += std::format("{}: {}", value_ref(fn, block.params[i]),
                                        type_name(fn.values[block.params[i]].type));
                }
                out += ")";
            }
            out += ":\n";
            for (const auto &inst : block.insts) {
                print_inst(out, module, fn, inst);
            }
        }

        out += "}\n";
        return out;
    }

    auto print(const Module &module) -> std::string {
        std::string out = std::format("; mirage ir — module '{}', {}-bit pointers\n\n",
                                       module.name, module.pointer_bits);

        for (const auto &global : module.globals) {
            out += std::format("{} @{}: size {}, align {}",
                                global.is_constant ? "const" : "global", global.name, global.size, global.align);
            if (global.linkage == Linkage::External) out += ", export";
            if (!global.section.empty()) out += std::format(", section(\"{}\")", global.section);
            if (global.init.empty()) {
                out += ", zeroinit";
            }
            out += "\n";
            for (const auto &reloc : global.relocations) {
                const auto target = reloc.kind == Relocation::Kind::FunctionAddr
                    ? (reloc.target < module.functions.size() ? module.functions[reloc.target].name : "<bad>")
                    : (reloc.target < module.globals.size() ? module.globals[reloc.target].name : "<bad>");
                out += std::format("  +{} -> @{}{}\n", reloc.offset, target,
                                    reloc.addend ? std::format(" + {}", reloc.addend) : "");
            }
        }
        if (!module.globals.empty()) out += "\n";

        for (const auto &fn : module.functions) {
            out += print(module, fn);
            out += "\n";
        }
        return out;
    }
}

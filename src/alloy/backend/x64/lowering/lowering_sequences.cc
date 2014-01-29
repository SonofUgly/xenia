/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <alloy/backend/x64/lowering/lowering_sequences.h>

#include <alloy/backend/x64/x64_emitter.h>
#include <alloy/backend/x64/x64_function.h>
#include <alloy/backend/x64/lowering/lowering_table.h>
#include <alloy/backend/x64/lowering/tracers.h>
#include <alloy/runtime/symbol_info.h>
#include <alloy/runtime/runtime.h>
#include <alloy/runtime/thread_state.h>

// TODO(benvanik): reimplement packing functions
#include <DirectXPackedVector.h>

using namespace alloy;
using namespace alloy::backend::x64;
using namespace alloy::backend::x64::lowering;
using namespace alloy::hir;
using namespace alloy::runtime;

using namespace Xbyak;

namespace {

#define UNIMPLEMENTED_SEQ() __debugbreak()
#define ASSERT_INVALID_TYPE() XEASSERTALWAYS()

#define ITRACE 1
#define DTRACE 1

#define SHUFPS_SWAP_DWORDS 0x1B

enum XmmConst {
  XMMZero               = 0,
  XMMOne                = 1,
  XMMNegativeOne        = 2,
  XMMMaskX16Y16         = 3,
  XMMFlipX16Y16         = 4,
  XMMFixX16Y16          = 5,
  XMMNormalizeX16Y16    = 6,
  XMM3301               = 7,
  XMMSignMaskPS         = 8,
  XMMSignMaskPD         = 9,
  XMMByteSwapMask       = 10,
};
static const vec128_t xmm_consts[] = {
  /* XMMZero                */ vec128f(0.0f, 0.0f, 0.0f, 0.0f),
  /* XMMOne                 */ vec128f(1.0f, 1.0f, 1.0f, 1.0f),
  /* XMMNegativeOne         */ vec128f(-1.0f, -1.0f, -1.0f, -1.0f),
  /* XMMMaskX16Y16          */ vec128i(0x0000FFFF, 0xFFFF0000, 0x00000000, 0x00000000),
  /* XMMFlipX16Y16          */ vec128i(0x00008000, 0x00000000, 0x00000000, 0x00000000),
  /* XMMFixX16Y16           */ vec128f(-32768.0f, 0.0f, 0.0f, 0.0f),
  /* XMMNormalizeX16Y16     */ vec128f(1.0f / 32767.0f, 1.0f / (32767.0f * 65536.0f), 0.0f, 0.0f),
  /* XMM3301                */ vec128f(3.0f, 3.0f, 0.0f, 1.0f),
  /* XMMSignMaskPS          */ vec128i(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
  /* XMMSignMaskPD          */ vec128i(0x80000000u, 0x00000000u, 0x80000000u, 0x00000000u),
  /* XMMByteSwapMask        */ vec128i(0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu),
};
// Use consts by first loading the base register then accessing memory:
// e.mov(e.rax, XMMCONSTBASE)
// e.andps(reg, XMMCONST(XMM3303))
// TODO(benvanik): find a way to do this without the base register.
#define XMMCONSTBASE (uint64_t)&xmm_consts[0]
#define XMMCONST(base_reg, name) e.ptr[base_reg + name * 16]

// A note about vectors:
// Alloy represents vectors as xyzw pairs, with indices 0123.
// XMM registers are xyzw pairs with indices 3210, making them more like wzyx.
// This makes things somewhat confusing. It'd be nice to just shuffle the
// registers around on load/store, however certain operations require that
// data be in the right offset.
// Basically, this identity must hold:
//   shuffle(vec, b00011011) -> {x,y,z,w} => {x,y,z,w}
// All indices and operations must respect that.
//
// Memory (big endian):
// [00 01 02 03] [04 05 06 07] [08 09 0A 0B] [0C 0D 0E 0F] (x, y, z, w)
// load into xmm register:
// [0F 0E 0D 0C] [0B 0A 09 08] [07 06 05 04] [03 02 01 00] (w, z, y, x)

void Dummy() {
  //
}

void UnimplementedExtern(void* raw_context, ExternFunction* extern_fn) {
  // TODO(benvanik): generate this thunk at runtime? or a shim?
  auto thread_state = *((ThreadState**)raw_context);
  extern_fn->Call(thread_state);
}

void Unpack_FLOAT16_2(void* raw_context, __m128& v) {
  uint32_t src = v.m128_i32[3];
  v.m128_f32[0] = DirectX::PackedVector::XMConvertHalfToFloat((uint16_t)src);
  v.m128_f32[1] = DirectX::PackedVector::XMConvertHalfToFloat((uint16_t)(src >> 16));
  v.m128_f32[2] = 0.0f;
  v.m128_f32[3] = 1.0f;
}

uint64_t LoadClock(void* raw_context) {
  LARGE_INTEGER counter;
  uint64_t time = 0;
  if (QueryPerformanceCounter(&counter)) {
    time = counter.QuadPart;
  }
  return time;
}

void CallNative(X64Emitter& e, void* target) {
  e.mov(e.rax, (uint64_t)target);
  e.call(e.rax);
  e.mov(e.rcx, e.qword[e.rsp + 0]);
  e.mov(e.rdx, e.qword[e.rcx + 8]); // membase
}

// TODO(benvanik): fancy stuff.
void* ResolveFunctionSymbol(void* raw_context, FunctionInfo* symbol_info) {
  // TODO(benvanik): generate this thunk at runtime? or a shim?
  auto thread_state = *((ThreadState**)raw_context);

  Function* fn = NULL;
  thread_state->runtime()->ResolveFunction(symbol_info->address(), &fn);
  XEASSERTNOTNULL(fn);
  XEASSERT(fn->type() == Function::USER_FUNCTION);
  auto x64_fn = (X64Function*)fn;
  return x64_fn->machine_code();
}
void* ResolveFunctionAddress(void* raw_context, uint64_t target_address) {
  // TODO(benvanik): generate this thunk at runtime? or a shim?
  auto thread_state = *((ThreadState**)raw_context);

  Function* fn = NULL;
  thread_state->runtime()->ResolveFunction(target_address, &fn);
  XEASSERTNOTNULL(fn);
  XEASSERT(fn->type() == Function::USER_FUNCTION);
  auto x64_fn = (X64Function*)fn;
  return x64_fn->machine_code();
}
void IssueCall(X64Emitter& e, FunctionInfo* symbol_info, uint32_t flags) {
  // If we are an extern function, we can directly insert a call.
  auto fn = symbol_info->function();
  if (fn && fn->type() == Function::EXTERN_FUNCTION) {
    auto extern_fn = (ExternFunction*)fn;
    if (extern_fn->handler()) {
      e.mov(e.rdx, (uint64_t)extern_fn->arg0());
      e.mov(e.r8, (uint64_t)extern_fn->arg1());
      e.mov(e.rax, (uint64_t)extern_fn->handler());
    } else {
      // Unimplemented - call dummy.
      e.mov(e.rdx, (uint64_t)extern_fn);
      e.mov(e.rax, (uint64_t)UnimplementedExtern);
    }
  } else {
    // Generic call, resolve address.
    // TODO(benvanik): caching/etc. For now this makes debugging easier.
    e.mov(e.rdx, (uint64_t)symbol_info);
    e.mov(e.rax, (uint64_t)ResolveFunctionSymbol);
    e.call(e.rax);
    e.mov(e.rcx, e.qword[e.rsp + 0]);
    e.mov(e.rdx, e.qword[e.rcx + 8]); // membase
  }
  if (flags & CALL_TAIL) {
    // TODO(benvanik): adjust stack?
    e.add(e.rsp, 0x40);
    e.jmp(e.rax);
  } else {
    e.call(e.rax);
    e.mov(e.rcx, e.qword[e.rsp + 0]);
    e.mov(e.rdx, e.qword[e.rcx + 8]); // membase
  }
}
void IssueCallIndirect(X64Emitter& e, Value* target, uint32_t flags) {
  Reg64 r;
  e.BeginOp(target, r, 0);
  if (r != e.rdx) {
    e.mov(e.rdx, r);
  }
  e.EndOp(r);
  e.mov(e.rax, (uint64_t)ResolveFunctionAddress);
  e.call(e.rax);
  e.mov(e.rcx, e.qword[e.rsp + 0]);
  e.mov(e.rdx, e.qword[e.rcx + 8]); // membase
  if (flags & CALL_TAIL) {
    // TODO(benvanik): adjust stack?
    e.add(e.rsp, 0x40);
    e.jmp(e.rax);
  } else {
    e.call(e.rax);
    e.mov(e.rcx, e.qword[e.rsp + 0]);
    e.mov(e.rdx, e.qword[e.rcx + 8]); // membase
  }
}

}  // namespace


// Major templating foo lives in here.
#include <alloy/backend/x64/lowering/op_utils.inl>


void alloy::backend::x64::lowering::RegisterSequences(LoweringTable* table) {
// --------------------------------------------------------------------------
// General
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_COMMENT, [](X64Emitter& e, Instr*& i) {
#if ITRACE
  // TODO(benvanik): pass through.
  // TODO(benvanik): don't just leak this memory.
  auto str = (const char*)i->src1.offset;
  auto str_copy = xestrdupa(str);
  e.mov(e.rdx, (uint64_t)str_copy);
  CallNative(e, TraceString);
#endif  // ITRACE
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_NOP, [](X64Emitter& e, Instr*& i) {
  // If we got this, chances are we want it.
  e.nop();
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Debugging
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_SOURCE_OFFSET, [](X64Emitter& e, Instr*& i) {
#if XE_DEBUG
  e.nop();
  e.nop();
  e.mov(e.eax, (uint32_t)i->src1.offset);
  e.nop();
  e.nop();
#endif  // XE_DEBUG

  e.MarkSourceOffset(i);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_DEBUG_BREAK, [](X64Emitter& e, Instr*& i) {
  // TODO(benvanik): insert a call to the debug break function to let the
  //     debugger know.
  e.db(0xCC);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_DEBUG_BREAK_TRUE, [](X64Emitter& e, Instr*& i) {
  e.inLocalLabel();
  CheckBoolean(e, i->src1.value);
  e.jz(".x", e.T_SHORT);
  // TODO(benvanik): insert a call to the debug break function to let the
  //     debugger know.
  e.db(0xCC);
  e.L(".x");
  e.outLocalLabel();
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_TRAP, [](X64Emitter& e, Instr*& i) {
  // TODO(benvanik): insert a call to the trap function to let the
  //     debugger know.
  e.db(0xCC);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_TRAP_TRUE, [](X64Emitter& e, Instr*& i) {
  e.inLocalLabel();
  CheckBoolean(e, i->src1.value);
  e.jz(".x", e.T_SHORT);
  // TODO(benvanik): insert a call to the trap function to let the
  //     debugger know.
  e.db(0xCC);
  e.L(".x");
  e.outLocalLabel();
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Calls
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_CALL, [](X64Emitter& e, Instr*& i) {
  IssueCall(e, i->src1.symbol_info, i->flags);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_CALL_TRUE, [](X64Emitter& e, Instr*& i) {
  e.inLocalLabel();
  CheckBoolean(e, i->src1.value);
  e.jz(".x", e.T_SHORT);
  IssueCall(e, i->src2.symbol_info, i->flags);
  e.L(".x");
  e.outLocalLabel();
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_CALL_INDIRECT, [](X64Emitter& e, Instr*& i) {
  IssueCallIndirect(e, i->src1.value, i->flags);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_CALL_INDIRECT_TRUE, [](X64Emitter& e, Instr*& i) {
  e.inLocalLabel();
  CheckBoolean(e, i->src1.value);
  e.jz(".x", e.T_SHORT);
  IssueCallIndirect(e, i->src2.value, i->flags);
  e.L(".x");
  e.outLocalLabel();
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_RETURN, [](X64Emitter& e, Instr*& i) {
  // If this is the last instruction in the last block, just let us
  // fall through.
  if (i->next || i->block->next) {
    e.jmp("epilog", CodeGenerator::T_NEAR);
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_RETURN_TRUE, [](X64Emitter& e, Instr*& i) {
  CheckBoolean(e, i->src1.value);
  e.jnz("epilog", CodeGenerator::T_NEAR);
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Branches
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_BRANCH, [](X64Emitter& e, Instr*& i) {
  auto target = i->src1.label;
  e.jmp(target->name, e.T_NEAR);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_BRANCH_TRUE, [](X64Emitter& e, Instr*& i) {
  CheckBoolean(e, i->src1.value);
  auto target = i->src2.label;
  e.jnz(target->name, e.T_NEAR);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_BRANCH_FALSE, [](X64Emitter& e, Instr*& i) {
  CheckBoolean(e, i->src1.value);
  auto target = i->src2.label;
  e.jz(target->name, e.T_NEAR);
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Types
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_ASSIGN, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntUnaryOp(
        e, i,
        [](X64Emitter& e, Instr& i, const Reg& dest_src) {
          // nop - the mov will have happened.
        });
  } else if (IsFloatType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsVecType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_CAST, [](X64Emitter& e, Instr*& i) {
  if (i->dest->type == INT32_TYPE) {
    if (i->src1.value->type == FLOAT32_TYPE) {
      Reg32 dest;
      Xmm src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      e.vmovd(dest, src);
      e.EndOp(dest, src);
    } else {
      UNIMPLEMENTED_SEQ();
    }
  } else if (i->dest->type == INT64_TYPE) {
    if (i->src1.value->type == FLOAT64_TYPE) {
      Reg64 dest;
      Xmm src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      e.vmovq(dest, src);
      e.EndOp(dest, src);
    } else {
      UNIMPLEMENTED_SEQ();
    }
  } else if (i->dest->type == FLOAT32_TYPE) {
    if (i->src1.value->type == INT32_TYPE) {
      Xmm dest;
      Reg32 src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      e.vmovd(dest, src);
      e.EndOp(dest, src);
    } else {
      UNIMPLEMENTED_SEQ();
    }
  } else if (i->dest->type == FLOAT64_TYPE) {
    if (i->src1.value->type == INT64_TYPE) {
      Xmm dest;
      Reg64 src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      e.vmovq(dest, src);
      e.EndOp(dest, src);
    } else {
      UNIMPLEMENTED_SEQ();
    }
  } else if (IsVecType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_ZERO_EXTEND, [](X64Emitter& e, Instr*& i) {
  if (i->Match(SIG_TYPE_I16, SIG_TYPE_I8)) {
    Reg16 dest;
    Reg8 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movzx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_I8)) {
    Reg32 dest;
    Reg8 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movzx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_I16)) {
    Reg32 dest;
    Reg16 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movzx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_I8)) {
    Reg64 dest;
    Reg8 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movzx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_I16)) {
    Reg64 dest;
    Reg16 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movzx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_I32)) {
    Reg64 dest;
    Reg32 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.mov(dest.cvt32(), src.cvt32());
    e.EndOp(dest, src);
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SIGN_EXTEND, [](X64Emitter& e, Instr*& i) {
  if (i->Match(SIG_TYPE_I16, SIG_TYPE_I8)) {
    Reg16 dest;
    Reg8 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movsx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_I8)) {
    Reg32 dest;
    Reg8 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movsx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_I16)) {
    Reg32 dest;
    Reg16 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movsx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_I8)) {
    Reg64 dest;
    Reg8 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movsx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_I16)) {
    Reg64 dest;
    Reg16 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movsx(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_I32)) {
    Reg64 dest;
    Reg32 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.movsxd(dest, src);
    e.EndOp(dest, src);
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_TRUNCATE, [](X64Emitter& e, Instr*& i) {
  if (i->Match(SIG_TYPE_I8, SIG_TYPE_I16)) {
    Reg8 dest;
    Reg16 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.mov(dest, src.cvt8());
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I8, SIG_TYPE_I32)) {
    Reg8 dest;
    Reg16 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.mov(dest, src.cvt8());
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I8, SIG_TYPE_I64)) {
    Reg8 dest;
    Reg64 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.mov(dest, src.cvt8());
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I16, SIG_TYPE_I32)) {
    Reg16 dest;
    Reg32 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.mov(dest, src.cvt16());
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I16, SIG_TYPE_I64)) {
    Reg16 dest;
    Reg64 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.mov(dest, src.cvt16());
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_I64)) {
    Reg32 dest;
    Reg64 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.mov(dest, src.cvt32());
    e.EndOp(dest, src);
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_CONVERT, [](X64Emitter& e, Instr*& i) {
  if (i->Match(SIG_TYPE_I32, SIG_TYPE_F32)) {
    Reg32 dest;
    Xmm src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    // TODO(benvanik): additional checks for saturation/etc? cvtt* (trunc?)
    e.cvttss2si(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_F64)) {
    Reg32 dest;
    Xmm src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    // TODO(benvanik): additional checks for saturation/etc? cvtt* (trunc?)
    e.cvtsd2ss(e.xmm0, src);
    e.cvttss2si(dest, e.xmm0);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_F64)) {
    Reg64 dest;
    Xmm src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    // TODO(benvanik): additional checks for saturation/etc? cvtt* (trunc?)
    e.cvttsd2si(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_F32, SIG_TYPE_I32)) {
    Xmm dest;
    Reg32 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    // TODO(benvanik): additional checks for saturation/etc?
    e.cvtsi2ss(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_F32, SIG_TYPE_F64)) {
    Xmm dest, src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    // TODO(benvanik): additional checks for saturation/etc?
    e.cvtsd2ss(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_F64, SIG_TYPE_I64)) {
    Xmm dest;
    Reg64 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    // TODO(benvanik): additional checks for saturation/etc?
    e.cvtsi2sd(dest, src);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_F64, SIG_TYPE_F32)) {
    Xmm dest, src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.cvtss2sd(dest, src);
    e.EndOp(dest, src);
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_ROUND, [](X64Emitter& e, Instr*& i) {
  // flags = ROUND_TO_*
  if (IsFloatType(i->dest->type)) {
    XmmUnaryOp(e, i, 0, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      if (i.src1.value->type == FLOAT32_TYPE) {
        switch (i.flags) {
        case ROUND_TO_ZERO:
          e.roundss(dest, src, B00000011);
          break;
        case ROUND_TO_NEAREST:
          e.roundss(dest, src, B00000000);
          break;
        case ROUND_TO_MINUS_INFINITY:
          e.roundss(dest, src, B00000001);
          break;
        case ROUND_TO_POSITIVE_INFINITY:
          e.roundss(dest, src, B00000010);
          break;
        }
      } else {
        switch (i.flags) {
        case ROUND_TO_ZERO:
          e.roundsd(dest, src, B00000011);
          break;
        case ROUND_TO_NEAREST:
          e.roundsd(dest, src, B00000000);
          break;
        case ROUND_TO_MINUS_INFINITY:
          e.roundsd(dest, src, B00000001);
          break;
        case ROUND_TO_POSITIVE_INFINITY:
          e.roundsd(dest, src, B00000010);
          break;
        }
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmUnaryOp(e, i, 0, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      switch (i.flags) {
      case ROUND_TO_ZERO:
        e.roundps(dest, src, B00000011);
        break;
      case ROUND_TO_NEAREST:
        e.roundps(dest, src, B00000000);
        break;
      case ROUND_TO_MINUS_INFINITY:
        e.roundps(dest, src, B00000001);
        break;
      case ROUND_TO_POSITIVE_INFINITY:
        e.roundps(dest, src, B00000010);
        break;
      }
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_CONVERT_I2F, [](X64Emitter& e, Instr*& i) {
  // flags = ARITHMETIC_UNSIGNED
  XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
    // TODO(benvanik): are these really the same? VC++ thinks so.
    if (i.flags & ARITHMETIC_UNSIGNED) {
      e.cvtdq2ps(dest, src);
    } else {
      e.cvtdq2ps(dest, src);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_CONVERT_F2I, [](X64Emitter& e, Instr*& i) {
  // flags = ARITHMETIC_SATURATE | ARITHMETIC_UNSIGNED
  XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
    // TODO(benvanik): are these really the same? VC++ thinks so.
    if (i.flags & ARITHMETIC_UNSIGNED) {
      e.cvttps2dq(dest, src);
    } else {
      e.cvttps2dq(dest, src);
    }
    if (i.flags & ARITHMETIC_SATURATE) {
      UNIMPLEMENTED_SEQ();
    }
  });
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

// specials for zeroing/etc (xor/etc)

table->AddSequence(OPCODE_LOAD_VECTOR_SHL, [](X64Emitter& e, Instr*& i) {
  XEASSERT(i->dest->type == VEC128_TYPE);
  UNIMPLEMENTED_SEQ();
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_LOAD_VECTOR_SHR, [](X64Emitter& e, Instr*& i) {
  XEASSERT(i->dest->type == VEC128_TYPE);
  UNIMPLEMENTED_SEQ();
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_LOAD_CLOCK, [](X64Emitter& e, Instr*& i) {
  // It'd be cool to call QueryPerformanceCounter directly, but w/e.
  CallNative(e, LoadClock);
  Reg64 dest;
  e.BeginOp(i->dest, dest, REG_DEST);
  e.mov(dest, e.rax);
  e.EndOp(dest);
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Context
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_LOAD_CONTEXT, [](X64Emitter& e, Instr*& i) {
  auto addr = e.rcx + i->src1.offset;
  if (i->Match(SIG_TYPE_I8, SIG_TYPE_IGNORE)) {
    Reg8 dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.mov(dest, e.byte[addr]);
    e.EndOp(dest);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8b, dest);
    CallNative(e, TraceContextLoadI8);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_I16, SIG_TYPE_IGNORE)) {
    Reg16 dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.mov(dest, e.word[addr]);
    e.EndOp(dest);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8w, dest);
    CallNative(e, TraceContextLoadI16);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_IGNORE)) {
    Reg32 dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.mov(dest, e.dword[addr]);
    e.EndOp(dest);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8d, dest);
    CallNative(e, TraceContextLoadI32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_IGNORE)) {
    Reg64 dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.mov(dest, e.qword[addr]);
    e.EndOp(dest);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8, dest);
    CallNative(e, TraceContextLoadI64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_F32, SIG_TYPE_IGNORE)) {
    Xmm dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.movss(dest, e.dword[addr]);
    e.EndOp(dest);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.lea(e.r8, Stash(e, dest));
    CallNative(e, TraceContextLoadF32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_F64, SIG_TYPE_IGNORE)) {
    Xmm dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.movsd(dest, e.qword[addr]);
    e.EndOp(dest);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.lea(e.r8, Stash(e, dest));
    CallNative(e, TraceContextLoadF64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_IGNORE)) {
    Xmm dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    // NOTE: we always know we are aligned.
    e.movaps(dest, e.ptr[addr]);
    e.EndOp(dest);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.lea(e.r8, Stash(e, dest));
    CallNative(e, TraceContextLoadV128);
#endif  // DTRACE
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_STORE_CONTEXT, [](X64Emitter& e, Instr*& i) {
  auto addr = e.rcx + i->src1.offset;
  if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I8)) {
    Reg8 src;
    e.BeginOp(i->src2.value, src, 0);
    e.mov(e.byte[addr], src);
    e.EndOp(src);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8b, src);
    CallNative(e, TraceContextStoreI8);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I8C)) {
    e.mov(e.byte[addr], i->src2.value->constant.i8);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8b, i->src2.value->constant.i8);
    CallNative(e, TraceContextStoreI8);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I16)) {
    Reg16 src;
    e.BeginOp(i->src2.value, src, 0);
    e.mov(e.word[addr], src);
    e.EndOp(src);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8w, src);
    CallNative(e, TraceContextStoreI16);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I16C)) {
    e.mov(e.word[addr], i->src2.value->constant.i16);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8w, i->src2.value->constant.i16);
    CallNative(e, TraceContextStoreI16);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I32)) {
    Reg32 src;
    e.BeginOp(i->src2.value, src, 0);
    e.mov(e.dword[addr], src);
    e.EndOp(src);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8d, src);
    CallNative(e, TraceContextStoreI32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I32C)) {
    e.mov(e.dword[addr], i->src2.value->constant.i32);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8d, i->src2.value->constant.i32);
    CallNative(e, TraceContextStoreI32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I64)) {
    Reg64 src;
    e.BeginOp(i->src2.value, src, 0);
    e.mov(e.qword[addr], src);
    e.EndOp(src);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8, src);
    CallNative(e, TraceContextStoreI64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I64C)) {
    MovMem64(e, addr, i->src2.value->constant.i64);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.r8, i->src2.value->constant.i64);
    CallNative(e, TraceContextStoreI64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_F32)) {
    Xmm src;
    e.BeginOp(i->src2.value, src, 0);
    e.movss(e.dword[addr], src);
    e.EndOp(src);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.lea(e.r8, Stash(e, src));
    CallNative(e, TraceContextStoreF32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_F32C)) {
    e.mov(e.dword[addr], i->src2.value->constant.i32);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.eax, i->src2.value->constant.i32);
    e.vmovd(e.xmm0, e.eax);
    e.lea(e.r8, Stash(e, e.xmm0));
    CallNative(e, TraceContextStoreF32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_F64)) {
    Xmm src;
    e.BeginOp(i->src2.value, src, 0);
    e.movsd(e.qword[addr], src);
    e.EndOp(src);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.lea(e.r8, Stash(e, src));
    CallNative(e, TraceContextStoreF64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_F64C)) {
    MovMem64(e, addr, i->src2.value->constant.i64);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.mov(e.rax, i->src2.value->constant.i64);
    e.vmovq(e.xmm0, e.rax);
    e.lea(e.r8, Stash(e, e.xmm0));
    CallNative(e, TraceContextStoreF64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_V128)) {
    Xmm src;
    e.BeginOp(i->src2.value, src, 0);
    // NOTE: we always know we are aligned.
    e.movaps(e.ptr[addr], src);
    e.EndOp(src);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.lea(e.r8, Stash(e, src));
    CallNative(e, TraceContextStoreV128);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_V128C)) {
    // TODO(benvanik): check zero
    // TODO(benvanik): correct order?
    MovMem64(e, addr, i->src2.value->constant.v128.low);
    MovMem64(e, addr + 8, i->src2.value->constant.v128.high);
#if DTRACE
    e.mov(e.rdx, i->src1.offset);
    e.lea(e.r8, e.ptr[addr]);
    CallNative(e, TraceContextStoreV128);
#endif  // DTRACE
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Memory
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_LOAD, [](X64Emitter& e, Instr*& i) {
  // If this is a constant address load, check to see if it's in a register
  // range. We'll also probably want a dynamic check for unverified loads.
  // So far, most games use constants.
  if (i->src1.value->IsConstant()) {
    uint64_t address = i->src1.value->AsUint64();
    auto cbs = e.runtime()->access_callbacks();
    while (cbs) {
      if (cbs->handles(cbs->context, address)) {
        // Eh, hacking lambdas.
        i->src3.offset = (uint64_t)cbs;
        IntUnaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src) {
          auto cbs = (RegisterAccessCallbacks*)i.src3.offset;
          e.mov(e.rcx, (uint64_t)cbs->context);
          e.mov(e.rdx, i.src1.value->AsUint64());
          CallNative(e, cbs->read);
          e.mov(dest_src, e.rax);
        });
        i = e.Advance(i);
        return true;
      }
      cbs = cbs->next;
    }
  }

  // TODO(benvanik): dynamic register access check.
  // mov reg, [membase + address.32]
  Reg64 addr_off;
  RegExp addr;
  if (i->src1.value->IsConstant()) {
    // TODO(benvanik): a way to do this without using a register.
    e.mov(e.eax, i->src1.value->AsUint32());
    addr = e.rdx + e.rax;
  } else {
    e.BeginOp(i->src1.value, addr_off, 0);
    e.mov(addr_off.cvt32(), addr_off.cvt32()); // trunc to 32bits
    addr = e.rdx + addr_off;
  }
  if (i->Match(SIG_TYPE_I8, SIG_TYPE_IGNORE)) {
    Reg8 dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.mov(dest, e.byte[addr]);
    e.EndOp(dest);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8b, dest);
    CallNative(e, TraceMemoryLoadI8);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_I16, SIG_TYPE_IGNORE)) {
    Reg16 dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.mov(dest, e.word[addr]);
    e.EndOp(dest);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8w, dest);
    CallNative(e, TraceMemoryLoadI16);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_IGNORE)) {
    Reg32 dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.mov(dest, e.dword[addr]);
    e.EndOp(dest);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8d, dest);
    CallNative(e, TraceMemoryLoadI32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_IGNORE)) {
    Reg64 dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.mov(dest, e.qword[addr]);
    e.EndOp(dest);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8, dest);
    CallNative(e, TraceMemoryLoadI64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_F32, SIG_TYPE_IGNORE)) {
    Xmm dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.movss(dest, e.dword[addr]);
    e.EndOp(dest);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.lea(e.r8, Stash(e, dest));
    CallNative(e, TraceMemoryLoadF32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_F64, SIG_TYPE_IGNORE)) {
    Xmm dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    e.movsd(dest, e.qword[addr]);
    e.EndOp(dest);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.lea(e.r8, Stash(e, dest));
    CallNative(e, TraceMemoryLoadF64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_IGNORE)) {
    Xmm dest;
    e.BeginOp(i->dest, dest, REG_DEST);
    // TODO(benvanik): we should try to stick to movaps if possible.
    e.movups(dest, e.ptr[addr]);
    e.EndOp(dest);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.lea(e.r8, Stash(e, dest));
    CallNative(e, TraceMemoryLoadV128);
#endif  // DTRACE
  } else {
    ASSERT_INVALID_TYPE();
  }
  if (!i->src1.value->IsConstant()) {
    e.EndOp(addr_off);
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_STORE, [](X64Emitter& e, Instr*& i) {
  // If this is a constant address store, check to see if it's in a
  // register range. We'll also probably want a dynamic check for
  // unverified stores. So far, most games use constants.
  if (i->src1.value->IsConstant()) {
    uint64_t address = i->src1.value->AsUint64();
    auto cbs = e.runtime()->access_callbacks();
    while (cbs) {
      if (cbs->handles(cbs->context, address)) {
        e.mov(e.rcx, (uint64_t)cbs->context);
        e.mov(e.rdx, address);
        if (i->src2.value->IsConstant()) {
          e.mov(e.r8, i->src2.value->AsUint64());
        } else {
          Reg64 src2;
          e.BeginOp(i->src2.value, src2, 0);
          switch (i->src2.value->type) {
          case INT8_TYPE:
            e.movzx(e.r8d, src2.cvt8());
            break;
          case INT16_TYPE:
            e.movzx(e.r8d, src2.cvt16());
            break;
          case INT32_TYPE:
            e.movzx(e.r8, src2.cvt32());
            break;
          case INT64_TYPE:
            e.mov(e.r8, src2);
            break;
          default: ASSERT_INVALID_TYPE(); break;
          }
          e.EndOp(src2);
        }
        // eh?
        e.bswap(e.r8);
        CallNative(e, cbs->write);
      }
      cbs = cbs->next;
    }
  }

  // TODO(benvanik): dynamic register access check
  // mov [membase + address.32], reg
  Reg64 addr_off;
  RegExp addr;
  if (i->src1.value->IsConstant()) {
    e.mov(e.eax, i->src1.value->AsUint32());
    addr = e.rdx + e.rax;
  } else {
    e.BeginOp(i->src1.value, addr_off, 0);
    e.mov(addr_off.cvt32(), addr_off.cvt32()); // trunc to 32bits
    addr = e.rdx + addr_off;
  }
  if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I8)) {
    Reg8 src;
    e.BeginOp(i->src2.value, src, 0);
    e.mov(e.byte[addr], src);
    e.EndOp(src);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8b, src);
    CallNative(e, TraceMemoryStoreI8);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I8C)) {
    e.mov(e.byte[addr], i->src2.value->constant.i8);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8b, i->src2.value->constant.i8);
    CallNative(e, TraceMemoryStoreI8);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I16)) {
    Reg16 src;
    e.BeginOp(i->src2.value, src, 0);
    e.mov(e.word[addr], src);
    e.EndOp(src);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8w, src);
    CallNative(e, TraceMemoryStoreI16);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I16C)) {
    e.mov(e.word[addr], i->src2.value->constant.i16);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8w, i->src2.value->constant.i16);
    CallNative(e, TraceMemoryStoreI16);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I32)) {
    Reg32 src;
    e.BeginOp(i->src2.value, src, 0);
    e.mov(e.dword[addr], src);
    e.EndOp(src);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8d, src);
    CallNative(e, TraceMemoryStoreI32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I32C)) {
    e.mov(e.dword[addr], i->src2.value->constant.i32);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8d, i->src2.value->constant.i32);
    CallNative(e, TraceMemoryStoreI32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I64)) {
    Reg64 src;
    e.BeginOp(i->src2.value, src, 0);
    e.mov(e.qword[addr], src);
    e.EndOp(src);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8, src);
    CallNative(e, TraceMemoryStoreI64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_I64C)) {
    MovMem64(e, addr, i->src2.value->constant.i64);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.r8, i->src2.value->constant.i64);
    CallNative(e, TraceMemoryStoreI64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_F32)) {
    Xmm src;
    e.BeginOp(i->src2.value, src, 0);
    e.movss(e.dword[addr], src);
    e.EndOp(src);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.lea(e.r8, Stash(e, src));
    CallNative(e, TraceMemoryStoreF32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_F32C)) {
    e.mov(e.dword[addr], i->src2.value->constant.i32);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.mov(e.eax, i->src2.value->constant.i32);
    e.vmovd(e.xmm0, e.eax);
    e.lea(e.r8, Stash(e, e.xmm0));
    CallNative(e, TraceMemoryStoreF32);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_F64)) {
    Xmm src;
    e.BeginOp(i->src2.value, src, 0);
    e.movsd(e.qword[addr], src);
    e.EndOp(src);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.lea(e.r8, Stash(e, src));
    CallNative(e, TraceMemoryStoreF64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_F64C)) {
    MovMem64(e, addr, i->src2.value->constant.i64);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.movsd(e.xmm0, e.ptr[addr]);
    CallNative(e, TraceMemoryStoreF64);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_V128)) {
    Xmm src;
    e.BeginOp(i->src2.value, src, 0);
    // TODO(benvanik): we should try to stick to movaps if possible.
    e.movups(e.ptr[addr], src);
    e.EndOp(src);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.lea(e.r8, Stash(e, src));
    CallNative(e, TraceMemoryStoreV128);
#endif  // DTRACE
  } else if (i->Match(SIG_TYPE_X, SIG_TYPE_IGNORE, SIG_TYPE_V128C)) {
    // TODO(benvanik): check zero
    // TODO(benvanik): correct order?
    MovMem64(e, addr, i->src2.value->constant.v128.low);
    MovMem64(e, addr + 8, i->src2.value->constant.v128.high);
#if DTRACE
    e.lea(e.rdx, e.ptr[addr]);
    e.lea(e.r8, e.ptr[addr]);
    CallNative(e, TraceMemoryStoreV128);
#endif  // DTRACE
  } else {
    ASSERT_INVALID_TYPE();
  }
  if (!i->src1.value->IsConstant()) {
    e.EndOp(addr_off);
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_PREFETCH, [](X64Emitter& e, Instr*& i) {
  UNIMPLEMENTED_SEQ();
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Comparisons
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_MAX, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsFloatType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      if (i.src1.value->type == FLOAT32_TYPE) {
        e.maxss(dest_src, src);
      } else {
        e.maxsd(dest_src, src);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      e.maxps(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_MIN, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsFloatType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      if (i.src1.value->type == FLOAT32_TYPE) {
        e.minss(dest_src, src);
      } else {
        e.minsd(dest_src, src);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      e.minps(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SELECT, [](X64Emitter& e, Instr*& i) {
  CheckBoolean(e, i->src1.value);
  if (IsIntType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsFloatType(i->dest->type) || IsVecType(i->dest->type)) {
    Xmm dest, src2, src3;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src2.value, src2, 0,
              i->src3.value, src3, 0);
    // TODO(benvanik): find a way to do this without branches.
    e.inLocalLabel();
    e.movaps(dest, src3);
    e.jz(".skip");
    e.movaps(dest, src2);
    e.L(".skip");
    e.outLocalLabel();
    e.EndOp(dest, src2, src3);
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_IS_TRUE, [](X64Emitter& e, Instr*& i) {
  CheckBoolean(e, i->src1.value);
  Reg8 dest;
  e.BeginOp(i->dest, dest, REG_DEST);
  e.setnz(dest);
  e.EndOp(dest);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_IS_FALSE, [](X64Emitter& e, Instr*& i) {
  CheckBoolean(e, i->src1.value);
  Reg8 dest;
  e.BeginOp(i->dest, dest, REG_DEST);
  e.setz(dest);
  e.EndOp(dest);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_EQ, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.sete(dest);
    } else {
      e.setne(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_NE, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.setne(dest);
    } else {
      e.sete(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_SLT, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.setl(dest);
    } else {
      e.setge(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_SLE, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.setle(dest);
    } else {
      e.setg(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_SGT, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.setg(dest);
    } else {
      e.setle(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_SGE, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.setge(dest);
    } else {
      e.setl(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_ULT, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.setb(dest);
    } else {
      e.setae(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_ULE, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.setbe(dest);
    } else {
      e.seta(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_UGT, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.seta(dest);
    } else {
      e.setbe(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_COMPARE_UGE, [](X64Emitter& e, Instr*& i) {
  CompareXX(e, i, [](X64Emitter& e, Reg8& dest, bool invert) {
    if (!invert) {
      e.setae(dest);
    } else {
      e.setb(dest);
    }
  });
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_DID_CARRY, [](X64Emitter& e, Instr*& i) {
  Reg8 dest;
  e.BeginOp(i->dest, dest, REG_DEST);
  e.setc(dest);
  e.EndOp(dest);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_DID_OVERFLOW, [](X64Emitter& e, Instr*& i) {
  Reg8 dest;
  e.BeginOp(i->dest, dest, REG_DEST);
  e.seto(dest);
  e.EndOp(dest);
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_DID_SATURATE, [](X64Emitter& e, Instr*& i) {
  UNIMPLEMENTED_SEQ();
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_COMPARE_EQ, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    VectorCompareXX(e, i, VECTOR_CMP_EQ, true);
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_COMPARE_SGT, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    VectorCompareXX(e, i, VECTOR_CMP_GT, true);
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_COMPARE_SGE, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    VectorCompareXX(e, i, VECTOR_CMP_GE, true);
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_COMPARE_UGT, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    VectorCompareXX(e, i, VECTOR_CMP_GT, false);
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_COMPARE_UGE, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    VectorCompareXX(e, i, VECTOR_CMP_GE, false);
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Math
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_ADD, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      e.add(dest_src, src);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.add(dest_src, src);
    });
  } else if (IsFloatType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      if (i.src1.value->type == FLOAT32_TYPE) {
        e.addss(dest_src, src);
      } else {
        e.addsd(dest_src, src);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      e.addps(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_ADD_CARRY, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    // dest = src1 + src2 + src3.i8
    IntTernaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src2, const Operand& src3) {
      Reg8 src3_8(src3.getIdx());
      if (src3.getIdx() <= 4) {
        e.mov(e.ah, src3_8);
      } else {
        e.mov(e.al, src3_8);
        e.mov(e.ah, e.al);
      }
      e.sahf();
      e.adc(dest_src, src2);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src2, uint32_t src3) {
      e.mov(e.eax, src3);
      e.mov(e.ah, e.al);
      e.sahf();
      e.adc(dest_src, src2);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src2, const Operand& src3) {
      Reg8 src3_8(src3.getIdx());
      if (src3.getIdx() <= 4) {
        e.mov(e.ah, src3_8);
      } else {
        e.mov(e.al, src3_8);
        e.mov(e.ah, e.al);
      }
      e.sahf();
      e.adc(dest_src, src2);
    });
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_ADD, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    if (i->flags == INT8_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == INT16_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == INT32_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == FLOAT32_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else {
      ASSERT_INVALID_TYPE();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SUB, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      e.sub(dest_src, src);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.sub(dest_src, src);
    });
  } else if (IsFloatType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      if (i.src1.value->type == FLOAT32_TYPE) {
        e.subss(dest_src, src);
      } else {
        e.subsd(dest_src, src);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      e.subps(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_MUL, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      // RAX = value, RDX = clobbered
      // TODO(benvanik): make the register allocator put dest_src in RAX?
      auto Nax = LIKE_REG(e.rax, dest_src);
      e.mov(Nax, dest_src);
      if (i.flags & ARITHMETIC_UNSIGNED) {
        e.mul(src);
      } else {
        e.imul(src);
      }
      e.mov(dest_src, Nax);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      // RAX = value, RDX = clobbered
      // TODO(benvanik): make the register allocator put dest_src in RAX?
      auto Nax = LIKE_REG(e.rax, dest_src);
      auto Ndx = LIKE_REG(e.rdx, dest_src);
      e.mov(Nax, dest_src);
      e.mov(Ndx, src);
      if (i.flags & ARITHMETIC_UNSIGNED) {
        e.mul(Ndx);
      } else {
        e.imul(Ndx);
      }
      e.mov(dest_src, Nax);
    });
  } else if (IsFloatType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      if (i.flags & ARITHMETIC_UNSIGNED) { UNIMPLEMENTED_SEQ(); }
      if (i.src1.value->type == FLOAT32_TYPE) {
        e.mulss(dest_src, src);
      } else {
        e.mulsd(dest_src, src);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      if (i.flags & ARITHMETIC_UNSIGNED) { UNIMPLEMENTED_SEQ(); }
      e.mulps(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_MUL_HI, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      // RAX = value, RDX = clobbered
      // TODO(benvanik): make the register allocator put dest_src in RAX?
      auto Nax = LIKE_REG(e.rax, dest_src);
      auto Ndx = LIKE_REG(e.rdx, dest_src);
      e.mov(Nax, dest_src);
      if (i.flags & ARITHMETIC_UNSIGNED) {
        e.mul(src);
      } else {
        e.imul(src);
      }
      e.mov(dest_src, Ndx);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      // RAX = value, RDX = clobbered
      // TODO(benvanik): make the register allocator put dest_src in RAX?
      auto Nax = LIKE_REG(e.rax, dest_src);
      auto Ndx = LIKE_REG(e.rdx, dest_src);
      e.mov(Nax, dest_src);
      e.mov(Ndx, src);
      if (i.flags & ARITHMETIC_UNSIGNED) {
        e.mul(Ndx);
      } else {
        e.imul(Ndx);
      }
      e.mov(dest_src, Ndx);
    });
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_DIV, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      // RAX = value, RDX = clobbered
      // TODO(benvanik): make the register allocator put dest_src in RAX?
      auto Nax = LIKE_REG(e.rax, dest_src);
      e.mov(Nax, dest_src);
      if (i.flags & ARITHMETIC_UNSIGNED) {
        e.div(src);
      } else {
        e.idiv(src);
      }
      e.mov(dest_src, Nax);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      // RAX = value, RDX = clobbered
      // TODO(benvanik): make the register allocator put dest_src in RAX?
      auto Nax = LIKE_REG(e.rax, dest_src);
      auto Ndx = LIKE_REG(e.rdx, dest_src);
      e.mov(Nax, dest_src);
      e.mov(Ndx, src);
      if (i.flags & ARITHMETIC_UNSIGNED) {
        e.div(Ndx);
      } else {
        e.idiv(Ndx);
      }
      e.mov(dest_src, Nax);
    });
  } else if (IsFloatType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      if (i.flags & ARITHMETIC_UNSIGNED) { UNIMPLEMENTED_SEQ(); }
      if (i.src1.value->type == FLOAT32_TYPE) {
        e.divss(dest_src, src);
      } else {
        e.divsd(dest_src, src);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      if (i.flags & ARITHMETIC_UNSIGNED) { UNIMPLEMENTED_SEQ(); }
      e.divps(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_MUL_ADD, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsFloatType(i->dest->type)) {
    XmmTernaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src2, const Xmm& src3) {
      if (i.dest->type == FLOAT32_TYPE) {
        e.vfmadd132ss(dest_src, src3, src2);
      } else {
        e.vfmadd132sd(dest_src, src3, src2);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmTernaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src2, const Xmm& src3) {
      e.vfmadd132ps(dest_src, src3, src2);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_MUL_SUB, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsFloatType(i->dest->type)) {
    XmmTernaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src2, const Xmm& src3) {
      if (i.dest->type == FLOAT32_TYPE) {
        e.vfmsub132ss(dest_src, src3, src2);
      } else {
        e.vfmsub132sd(dest_src, src3, src2);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmTernaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src2, const Xmm& src3) {
      e.vfmsub132ps(dest_src, src3, src2);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_NEG, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntUnaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src) {
      e.neg(dest_src);
    });
  } else if (IsFloatType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      if (i.src1.value->type == FLOAT32_TYPE) {
        e.mov(e.rax, XMMCONSTBASE);
        e.vpxor(dest, src, XMMCONST(e.rax, XMMSignMaskPS));
      } else {
        e.mov(e.rax, XMMCONSTBASE);
        e.vpxor(dest, src, XMMCONST(e.rax, XMMSignMaskPD));
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      e.mov(e.rax, XMMCONSTBASE);
      e.vpxor(dest, src, XMMCONST(e.rax, XMMSignMaskPS));
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_ABS, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsFloatType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      if (i.src1.value->type == FLOAT32_TYPE) {
        e.mov(e.rax, XMMCONSTBASE);
        e.movaps(e.xmm0, XMMCONST(e.rax, XMMSignMaskPS));
        e.vpandn(dest, e.xmm0, src);
      } else {
        e.mov(e.rax, XMMCONSTBASE);
        e.movaps(e.xmm0, XMMCONST(e.rax, XMMSignMaskPD));;
        e.vpandn(dest, e.xmm0, src);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      e.mov(e.rax, XMMCONSTBASE);
      e.movaps(e.xmm0, XMMCONST(e.rax, XMMSignMaskPS));;
      e.vpandn(dest, e.xmm0, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SQRT, [](X64Emitter& e, Instr*& i) {
  if (IsFloatType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      if (i.dest->type == FLOAT32_TYPE) {
        e.sqrtss(dest, src);
      } else {
        e.sqrtsd(dest, src);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      e.sqrtps(dest, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_RSQRT, [](X64Emitter& e, Instr*& i) {
  if (IsFloatType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      if (i.dest->type == FLOAT32_TYPE) {
        e.rsqrtss(dest, src);
      } else {
        e.cvtsd2ss(dest, src);
        e.rsqrtss(dest, dest);
        e.cvtss2sd(dest, dest);
      }
    });
  } else if (IsVecType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      e.rsqrtps(dest, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_POW2, [](X64Emitter& e, Instr*& i) {
  if (IsFloatType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsVecType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_LOG2, [](X64Emitter& e, Instr*& i) {
  if (IsFloatType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else if (IsVecType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_DOT_PRODUCT_3, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->src1.value->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      // http://msdn.microsoft.com/en-us/library/bb514054(v=vs.90).aspx
      // TODO(benvanik): verify ordering
      e.dpps(dest_src, src, B01110001);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_DOT_PRODUCT_4, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->src1.value->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      // http://msdn.microsoft.com/en-us/library/bb514054(v=vs.90).aspx
      // TODO(benvanik): verify ordering
      e.dpps(dest_src, src, B11110001);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_AND, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      e.and(dest_src, src);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.and(dest_src, src);
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      e.pand(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_OR, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      e.or(dest_src, src);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.or(dest_src, src);
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      e.por(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_XOR, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      e.xor(dest_src, src);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.xor(dest_src, src);
    });
  } else if (IsVecType(i->dest->type)) {
    XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
      e.pxor(dest_src, src);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_NOT, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntUnaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src) {
      e.not(dest_src);
    });
  } else if (IsVecType(i->dest->type)) {
    XmmUnaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      // dest_src ^= 0xFFFF...
      e.cmpeqps(e.xmm0, e.xmm0);
      if (dest != src) {
        e.movaps(dest, src);
      }
      e.pxor(dest, e.xmm0);
    });
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SHL, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    // TODO(benvanik): use shlx if available.
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      // Can only shl by cl. Eww x86.
      Reg8 shamt(src.getIdx());
      e.mov(e.rax, e.rcx);
      e.mov(e.cl, shamt);
      e.shl(dest_src, e.cl);
      e.mov(e.rcx, e.rax);
      // BeaEngine can't disasm this, boo.
      /*Reg32e dest_src_e(dest_src.getIdx(), MAX(dest_src.getBit(), 32));
      Reg32e src_e(src.getIdx(), MAX(dest_src.getBit(), 32));
      e.and(src_e, 0x3F);
      e.shlx(dest_src_e, dest_src_e, src_e);*/
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.shl(dest_src, src);
    });
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SHR, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    // TODO(benvanik): use shrx if available.
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      // Can only sar by cl. Eww x86.
      Reg8 shamt(src.getIdx());
      e.mov(e.rax, e.rcx);
      e.mov(e.cl, shamt);
      e.shr(dest_src, e.cl);
      e.mov(e.rcx, e.rax);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.shr(dest_src, src);
    });
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SHA, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    // TODO(benvanik): use sarx if available.
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      // Can only sar by cl. Eww x86.
      Reg8 shamt(src.getIdx());
      e.mov(e.rax, e.rcx);
      e.mov(e.cl, shamt);
      e.sar(dest_src, e.cl);
      e.mov(e.rcx, e.rax);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.sar(dest_src, src);
    });
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_SHL, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    if (i->flags == INT8_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == INT16_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == INT32_TYPE) {
      XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
        // src shift mask may have values >31, and x86 sets to zero when
        // that happens so we mask.
        e.mov(e.eax, 0x1F);
        e.vmovd(e.xmm0, e.eax);
        e.vpbroadcastd(e.xmm0, e.xmm0);
        e.vandps(e.xmm0, src, e.xmm0);
        e.vpsllvd(dest_src, dest_src, e.xmm0);
      });
    } else {
      ASSERT_INVALID_TYPE();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_SHR, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    if (i->flags == INT8_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == INT16_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == INT32_TYPE) {
      XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
        // src shift mask may have values >31, and x86 sets to zero when
        // that happens so we mask.
        e.mov(e.eax, 0x1F);
        e.vmovd(e.xmm0, e.eax);
        e.vpbroadcastd(e.xmm0, e.xmm0);
        e.vandps(e.xmm0, src, e.xmm0);
        e.vpsrlvd(dest_src, dest_src, src);
      });
    } else {
      ASSERT_INVALID_TYPE();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_VECTOR_SHA, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    if (i->flags == INT8_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == INT16_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->flags == INT32_TYPE) {
      XmmBinaryOp(e, i, i->flags, [](X64Emitter& e, Instr& i, const Xmm& dest_src, const Xmm& src) {
        // src shift mask may have values >31, and x86 sets to zero when
        // that happens so we mask.
        e.mov(e.eax, 0x1F);
        e.vmovd(e.xmm0, e.eax);
        e.vpbroadcastd(e.xmm0, e.xmm0);
        e.vandps(e.xmm0, src, e.xmm0);
        e.vpsravd(dest_src, dest_src, src);
      });
    } else {
      ASSERT_INVALID_TYPE();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_ROTATE_LEFT, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    IntBinaryOp(e, i, [](X64Emitter& e, Instr& i, const Reg& dest_src, const Operand& src) {
      // Can only rol by cl. Eww x86.
      Reg8 shamt(src.getIdx());
      e.mov(e.rax, e.rcx);
      e.mov(e.cl, shamt);
      e.rol(dest_src, e.cl);
      e.mov(e.rcx, e.rax);
    }, [](X64Emitter& e, Instr& i, const Reg& dest_src, uint32_t src) {
      e.rol(dest_src, src);
    });
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_BYTE_SWAP, [](X64Emitter& e, Instr*& i) {
  if (i->Match(SIG_TYPE_I16, SIG_TYPE_I16)) {
    Reg16 dest, src1;
    // TODO(benvanik): fix register allocator to put the value in ABCD
    //e.BeginOp(i->dest, d, REG_DEST | REG_ABCD,
    //          i->src1.value, s1, 0);
    //if (d != s1) {
    //  e.mov(d, s1);
    //  e.xchg(d.cvt8(), Reg8(d.getIdx() + 4));
    //} else {
    //  e.xchg(d.cvt8(), Reg8(d.getIdx() + 4));
    //}
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src1, 0);
    e.mov(e.ax, src1);
    e.xchg(e.ah, e.al);
    e.mov(dest, e.ax);
    e.EndOp(dest, src1);
  } else if (i->Match(SIG_TYPE_I32, SIG_TYPE_I32)) {
    Reg32 dest, src1;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src1, 0);
    if (dest != src1) {
      e.mov(dest, src1);
      e.bswap(dest);
    } else {
      e.bswap(dest);
    }
    e.EndOp(dest, src1);
  } else if (i->Match(SIG_TYPE_I64, SIG_TYPE_I64)) {
    Reg64 dest, src1;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src1, 0);
    if (dest != src1) {
      e.mov(dest, src1);
      e.bswap(dest);
    } else {
      e.bswap(dest);
    }
    e.EndOp(dest, src1);
  } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_V128)) {
    Xmm dest, src1;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src1, 0);
    // TODO(benvanik): find a way to do this without the memory load.
    e.mov(e.rax, XMMCONSTBASE);
    e.vpshufb(dest, src1, XMMCONST(e.rax, XMMByteSwapMask));
    e.EndOp(dest, src1);
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_CNTLZ, [](X64Emitter& e, Instr*& i) {
  if (i->Match(SIG_TYPE_IGNORE, SIG_TYPE_I8)) {
    Reg8 dest;
    Reg8 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.bsr(dest.cvt16(), src.cvt16());
    // ZF = 1 if zero
    e.mov(e.eax, 16);
    e.cmovz(dest.cvt32(), e.eax);
    e.sub(dest, 8);
    e.xor(dest, 0x7);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_IGNORE, SIG_TYPE_I16)) {
    Reg8 dest;
    Reg16 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.bsr(dest.cvt16(), src);
    // ZF = 1 if zero
    e.mov(e.eax, 16);
    e.cmovz(dest.cvt32(), e.eax);
    e.xor(dest, 0xF);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_IGNORE, SIG_TYPE_I32)) {
    Reg8 dest;
    Reg32 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.bsr(dest.cvt32(), src);
    // ZF = 1 if zero
    e.mov(e.eax, 32);
    e.cmovz(dest.cvt32(), e.eax);
    e.xor(dest, 0x1F);
    e.EndOp(dest, src);
  } else if (i->Match(SIG_TYPE_IGNORE, SIG_TYPE_I64)) {
    Reg8 dest;
    Reg64 src;
    e.BeginOp(i->dest, dest, REG_DEST,
              i->src1.value, src, 0);
    e.bsr(dest, src);
    // ZF = 1 if zero
    e.mov(e.eax, 64);
    e.cmovz(dest.cvt32(), e.eax);
    e.xor(dest, 0x3F);
    e.EndOp(dest, src);
  } else {
    UNIMPLEMENTED_SEQ();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_INSERT, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    if (i->src3.value->type == INT8_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->src3.value->type == INT16_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else if (i->src3.value->type == INT32_TYPE) {
      UNIMPLEMENTED_SEQ();
    } else {
      ASSERT_INVALID_TYPE();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

// TODO(benvanik): sequence extract/splat:
//  v0.i32 = extract v0.v128, 0
//  v0.v128 = splat v0.i32
// This can be a single broadcast.

table->AddSequence(OPCODE_EXTRACT, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->src1.value->type)) {
    if (i->dest->type == INT8_TYPE) {
      Reg8 dest;
      Xmm src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      if (i->src2.value->IsConstant()) {
        e.pextrb(dest, src, i->src2.value->constant.i8);
      } else {
        UNIMPLEMENTED_SEQ();
      }
      e.EndOp(dest, src);
    } else if (i->dest->type == INT16_TYPE) {
      Reg16 dest;
      Xmm src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      if (i->src2.value->IsConstant()) {
        e.pextrw(dest, src, i->src2.value->constant.i8);
      } else {
        UNIMPLEMENTED_SEQ();
      }
      e.EndOp(dest, src);
    } else if (i->dest->type == INT32_TYPE) {
      Reg32 dest;
      Xmm src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      if (i->src2.value->IsConstant()) {
        e.pextrd(dest, src, i->src2.value->constant.i8);
      } else {
        UNIMPLEMENTED_SEQ();
      }
      e.EndOp(dest, src);
    } else if (i->dest->type == FLOAT32_TYPE) {
      Reg32 dest;
      Xmm src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      if (i->src2.value->IsConstant()) {
        e.extractps(dest, src, i->src2.value->constant.i8);
      } else {
        UNIMPLEMENTED_SEQ();
      }
      e.EndOp(dest, src);
    } else {
      ASSERT_INVALID_TYPE();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SPLAT, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    if (i->Match(SIG_TYPE_V128, SIG_TYPE_I8)) {
      Xmm dest;
      Reg8 src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      e.vmovd(e.xmm0, src.cvt32());
      e.vpbroadcastb(dest, e.xmm0);
      e.EndOp(dest, src);
    } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_I8C)) {
      Xmm dest;
      e.BeginOp(i->dest, dest, REG_DEST);
      // TODO(benvanik): faster constant splats.
      e.mov(e.eax, i->src1.value->constant.i8);
      e.vmovd(e.xmm0, e.eax);
      e.vpbroadcastb(dest, e.xmm0);
      e.EndOp(dest);
    } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_I16)) {
      Xmm dest;
      Reg16 src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      e.vmovd(e.xmm0, src.cvt32());
      e.vpbroadcastw(dest, e.xmm0);
      e.EndOp(dest, src);
    } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_I16C)) {
      Xmm dest;
      e.BeginOp(i->dest, dest, REG_DEST);
      // TODO(benvanik): faster constant splats.
      e.mov(e.eax, i->src1.value->constant.i16);
      e.vmovd(e.xmm0, e.eax);
      e.vpbroadcastw(dest, e.xmm0);
      e.EndOp(dest);
    } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_I32)) {
      Xmm dest;
      Reg32 src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      e.vmovd(e.xmm0, src);
      e.vpbroadcastd(dest, e.xmm0);
      e.EndOp(dest, src);
    } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_I32C)) {
      Xmm dest;
      e.BeginOp(i->dest, dest, REG_DEST);
      // TODO(benvanik): faster constant splats.
      e.mov(e.eax, i->src1.value->constant.i32);
      e.vmovd(e.xmm0, e.eax);
      e.vpbroadcastd(dest, e.xmm0);
      e.EndOp(dest);
    } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_F32)) {
      Xmm dest, src;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src, 0);
      e.vbroadcastss(dest, src);
      e.EndOp(dest, src);
    } else if (i->Match(SIG_TYPE_V128, SIG_TYPE_F32C)) {
      Xmm dest;
      e.BeginOp(i->dest, dest, REG_DEST);
      e.mov(e.eax, i->src1.value->constant.i32);
      e.vmovd(e.xmm0, e.eax);
      e.vbroadcastss(dest, e.xmm0);
      e.EndOp(dest);
    } else {
      ASSERT_INVALID_TYPE();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_PERMUTE, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    if (i->src1.value->type == INT32_TYPE) {
      // Permute words between src2 and src3.
      // TODO(benvanik): check src3 for zero. if 0, we can use pshufb.
      if (i->src1.value->IsConstant()) {
        uint32_t control = i->src1.value->AsUint32();
        Xmm dest, src2, src3;
        e.BeginOp(i->dest, dest, REG_DEST,
                  i->src2.value, src2, 0,
                  i->src3.value, src3, 0);
        // Shuffle things into the right places in dest & xmm0,
        // then we blend them together.
        uint32_t src_control =
            (((control >> 24) & 0x3) << 0) |
            (((control >> 16) & 0x3) << 2) |
            (((control >> 8) & 0x3) << 4) |
            (((control >> 0) & 0x3) << 6);
        uint32_t blend_control =
            (((control >> 26) & 0x1) << 0) |
            (((control >> 18) & 0x1) << 1) |
            (((control >> 10) & 0x1) << 2) |
            (((control >> 2) & 0x1) << 3);
        if (dest != src3) {
          e.pshufd(dest, src2, src_control);
          e.pshufd(e.xmm0, src3, src_control);
          e.blendps(dest, e.xmm0, blend_control);
        } else {
          e.movaps(e.xmm0, src3);
          e.pshufd(dest, src2, src_control);
          e.pshufd(e.xmm0, e.xmm0, src_control);
          e.blendps(dest, e.xmm0, blend_control);
        }
        e.EndOp(dest, src2, src3);
      } else {
        Reg32 control;
        Xmm dest, src2, src3;
        e.BeginOp(i->dest, dest, REG_DEST,
                  i->src1.value, control, 0,
                  i->src2.value, src2, 0,
                  i->src3.value, src3, 0);
        UNIMPLEMENTED_SEQ();
        e.EndOp(dest, control, src2, src3);
      }
    } else if (i->src1.value->type == VEC128_TYPE) {
      // Permute bytes between src2 and src3.
      // TODO(benvanik): check src3 for zero. if 0, we can use pshufb.
      Xmm dest, control, src2, src3;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, control, 0,
                i->src2.value, src2, 0,
                i->src3.value, src3, 0);
      UNIMPLEMENTED_SEQ();
      e.EndOp(dest, control, src2, src3);
    } else {
      ASSERT_INVALID_TYPE();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_SWIZZLE, [](X64Emitter& e, Instr*& i) {
  if (IsVecType(i->dest->type)) {
    // Defined by SWIZZLE_MASK()
    if (i->flags == INT32_TYPE || i->flags == FLOAT32_TYPE) {
      uint8_t swizzle_mask = (uint8_t)i->src2.offset;
      swizzle_mask =
          (((swizzle_mask >> 6) & 0x3) << 0) |
          (((swizzle_mask >> 4) & 0x3) << 2) |
          (((swizzle_mask >> 2) & 0x3) << 4) |
          (((swizzle_mask >> 0) & 0x3) << 6);
      Xmm dest, src1;
      e.BeginOp(i->dest, dest, REG_DEST,
                i->src1.value, src1, 0);
      e.pshufd(dest, src1, swizzle_mask);
      e.EndOp(dest, src1);
    } else {
      UNIMPLEMENTED_SEQ();
    }
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_PACK, [](X64Emitter& e, Instr*& i) {
  if (i->flags == PACK_TYPE_D3DCOLOR) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_FLOAT16_2) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_FLOAT16_4) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_SHORT_2) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_S8_IN_16_LO) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_S8_IN_16_HI) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_S16_IN_32_LO) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_S16_IN_32_HI) {
    UNIMPLEMENTED_SEQ();
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_UNPACK, [](X64Emitter& e, Instr*& i) {
  if (i->flags == PACK_TYPE_D3DCOLOR) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_FLOAT16_2) {
    // 1 bit sign, 5 bit exponent, 10 bit mantissa
    // D3D10 half float format
    // TODO(benvanik): http://blogs.msdn.com/b/chuckw/archive/2012/09/11/directxmath-f16c-and-fma.aspx
    // Use _mm_cvtph_ps -- requires very modern processors (SSE5+)
    // Unpacking half floats: http://fgiesen.wordpress.com/2012/03/28/half-to-float-done-quic/
    // Packing half floats: https://gist.github.com/rygorous/2156668
    // Load source, move from tight pack of X16Y16.... to X16...Y16...
    // Also zero out the high end.
    // TODO(benvanik): special case constant unpacks that just get 0/1/etc.
    XmmUnaryOp(e, i, 0, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      // sx = src.iw >> 16;
      // sy = src.iw & 0xFFFF;
      // dest = { XMConvertHalfToFloat(sx),
      //          XMConvertHalfToFloat(sy),
      //          0.0,
      //          1.0 };
      auto addr = Stash(e, src);
      e.lea(e.rdx, addr);
      CallNative(e, Unpack_FLOAT16_2);
      e.movaps(dest, addr);
    });
  } else if (i->flags == PACK_TYPE_FLOAT16_4) {
    // Could be shared with FLOAT16_2.
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_SHORT_2) {
    // (VD.x) = 3.0 + (VB.x>>16)*2^-22
    // (VD.y) = 3.0 + (VB.x)*2^-22
    // (VD.z) = 0.0
    // (VD.w) = 1.0
    XmmUnaryOp(e, i, 0, [](X64Emitter& e, Instr& i, const Xmm& dest, const Xmm& src) {
      // XMLoadShortN2 plus 3,3,0,3 (for some reason)
      // src is (xx,xx,xx,VALUE)
      e.mov(e.rax, XMMCONSTBASE);
      // (VALUE,VALUE,VALUE,VALUE)
      e.vbroadcastss(dest, src);
      // (VALUE&0xFFFF,VALUE&0xFFFF0000,0,0)
      e.andps(dest, XMMCONST(e.rax, XMMMaskX16Y16));
      // Sign extend.
      e.xorps(dest, XMMCONST(e.rax, XMMFlipX16Y16));
      // Convert int->float.
      e.cvtpi2ps(dest, Stash(e, dest));
      // 0x8000 to undo sign.
      e.addps(dest, XMMCONST(e.rax, XMMFixX16Y16));
      // Normalize.
      e.mulps(dest, XMMCONST(e.rax, XMMNormalizeX16Y16));
      // Clamp.
      e.maxps(dest, XMMCONST(e.rax, XMMNegativeOne));
      // Add 3,3,0,1.
      e.addps(dest, XMMCONST(e.rax, XMM3301));
    });
  } else if (i->flags == PACK_TYPE_S8_IN_16_LO) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_S8_IN_16_HI) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_S16_IN_32_LO) {
    UNIMPLEMENTED_SEQ();
  } else if (i->flags == PACK_TYPE_S16_IN_32_HI) {
    UNIMPLEMENTED_SEQ();
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

// --------------------------------------------------------------------------
// Atomic
// --------------------------------------------------------------------------

table->AddSequence(OPCODE_COMPARE_EXCHANGE, [](X64Emitter& e, Instr*& i) {
  UNIMPLEMENTED_SEQ();
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_ATOMIC_EXCHANGE, [](X64Emitter& e, Instr*& i) {
  if (IsIntType(i->dest->type)) {
    UNIMPLEMENTED_SEQ();
  } else {
    ASSERT_INVALID_TYPE();
  }
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_ATOMIC_ADD, [](X64Emitter& e, Instr*& i) {
  UNIMPLEMENTED_SEQ();
  i = e.Advance(i);
  return true;
});

table->AddSequence(OPCODE_ATOMIC_SUB, [](X64Emitter& e, Instr*& i) {
  UNIMPLEMENTED_SEQ();
  i = e.Advance(i);
  return true;
});
}

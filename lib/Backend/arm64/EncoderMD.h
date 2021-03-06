//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "ARM64Encoder.h"
#include "LegalizeMD.h"

class Encoder;

enum RelocType {
    RelocTypeBranch14,
    RelocTypeBranch19,
    RelocTypeBranch26,
    RelocTypeDataLabelLow,
    RelocTypeLabelLow,
    RelocTypeLabelHigh,
    RelocTypeLabel
};

enum InstructionType {
    None    = 0,
    Vfp     = 3,
    A64     = 4,
};

#define RETURN_REG          RegR0
#define FIRST_INT_ARG_REG   RegR0
#define LAST_INT_ARG_REG    RegR7
#define NUM_INT_ARG_REGS\
    ((LAST_INT_ARG_REG - FIRST_INT_ARG_REG) + 1)
#define FIRST_CALLEE_SAVED_GP_REG RegR19
#define LAST_CALLEE_SAVED_GP_REG  RegR28
#define SCRATCH_REG         RegR17
#define ALT_LOCALS_PTR      RegR21
#define EH_STACK_SAVE_REG   RegR20
#define SP_ALLOC_SCRATCH_REG RegR18
#define CATCH_OBJ_REG       RegR1

#define RETURN_DBL_REG      RegD0
#define FIRST_CALLEE_SAVED_DBL_REG RegD16
#define LAST_CALLEE_SAVED_DBL_REG  RegD29
#define FIRST_CALLEE_SAVED_DBL_REG_NUM 16
#define LAST_CALLEE_SAVED_DBL_REG_NUM 29
#define CALLEE_SAVED_DOUBLE_REG_COUNT 16

// See comment in LowerEntryInstr: even in a global function, we'll home r0 and r1
#define MIN_HOMED_PARAM_REGS 2

#define FRAME_REG           RegFP

//
// Opcode dope
//

#define DMOVE              0x0001
#define DLOAD              0x0002
#define DSTORE             0x0003
#define DMASK              0x0007
#define DHALFORSB          0x0020    // halfword or signed byte
#define DSUPDATE           0x0100
#define DSSUB              0x0200
#define DSPOST             0x0400
#define DSBIT              0x0800

#define D___               (0)
#define D__S               (DSBIT)
#define DM__               (DMOVE)
#define DL__               (DLOAD)
#define DLH_               (DLOAD | DHALFORSB)
#define DS__               (DSTORE)
#define DSH_               (DSTORE | DHALFORSB)
#define DLUP               (DLOAD | DSUPDATE | DSPOST)
#define DSUS               (DSTORE | DSUPDATE | DSSUB)

#define ISMOVE(o)          ((EncoderMD::GetOpdope(o) & DMASK) == DMOVE)
#define ISLOAD(o)          ((EncoderMD::GetOpdope(o) & DMASK) == DLOAD)
#define ISSTORE(o)         ((EncoderMD::GetOpdope(o) & DMASK) == DSTORE)

#define ISSHIFTERUPDATE(o) ((EncoderMD::GetOpdope(o) & DSUPDATE) != 0)
#define ISSHIFTERSUB(o)    ((EncoderMD::GetOpdope(o) & DSSUB) != 0)
#define ISSHIFTERPOST(o)   ((EncoderMD::GetOpdope(o) & DSPOST) != 0)

#define SETS_SBIT(o)       ((EncoderMD::GetOpdope(o) & DSBIT) != 0)

#define IS_CONST_0000003F(x) (((x) & ~0x0000003f) == 0)
#define IS_CONST_0000007F(x) (((x) & ~0x0000007f) == 0)
#define IS_CONST_000000FF(x) (((x) & ~0x000000ff) == 0)
#define IS_CONST_000003FF(x) (((x) & ~0x000003ff) == 0)
#define IS_CONST_00000FFF(x) (((x) & ~0x00000fff) == 0)
#define IS_CONST_0000FFFF(x) (((x) & ~0x0000ffff) == 0)
#define IS_CONST_000FFFFF(x) (((x) & ~0x000fffff) == 0)
#define IS_CONST_007FFFFF(x) (((x) & ~0x007fffff) == 0)

#define IS_CONST_NEG_7(x)    (((x) & ~0x0000003f) == ~0x0000003f)
#define IS_CONST_NEG_8(x)    (((x) & ~0x0000007f) == ~0x0000007f)
#define IS_CONST_NEG_9(x)    (((x) & ~0x000000ff) == ~0x000000ff)
#define IS_CONST_NEG_12(x)   (((x) & ~0x000007ff) == ~0x000007ff)
#define IS_CONST_NEG_21(x)   (((x) & ~0x000fffff) == ~0x000fffff)
#define IS_CONST_NEG_24(x)   (((x) & ~0x007fffff) == ~0x007fffff)

#define IS_CONST_INT7(x)     (IS_CONST_0000003F(x) || IS_CONST_NEG_7(x))
#define IS_CONST_INT8(x)     (IS_CONST_0000007F(x) || IS_CONST_NEG_8(x))
#define IS_CONST_INT9(x)     (IS_CONST_000000FF(x) || IS_CONST_NEG_9(x))
#define IS_CONST_INT12(x)    (IS_CONST_00000FFF(x) || IS_CONST_NEG_12(x))
#define IS_CONST_INT21(x)    (IS_CONST_000FFFFF(x) || IS_CONST_NEG_21(x))
#define IS_CONST_INT24(x)    (IS_CONST_007FFFFF(x) || IS_CONST_NEG_24(x))

#define IS_CONST_UINT6(x)    IS_CONST_0000003F(x)
#define IS_CONST_UINT7(x)    IS_CONST_0000007F(x)
#define IS_CONST_UINT8(x)    IS_CONST_000000FF(x)
#define IS_CONST_UINT10(x)   IS_CONST_000003FF(x)
#define IS_CONST_UINT12(x)   IS_CONST_00000FFF(x)
#define IS_CONST_UINT16(x)   IS_CONST_0000FFFF(x)


///---------------------------------------------------------------------------
///
/// class EncoderReloc
///
///---------------------------------------------------------------------------

class EncodeReloc
{
public:
    static void     New(EncodeReloc **pHead, RelocType relocType, BYTE *offset, IR::Instr *relocInstr, ArenaAllocator *alloc);

public:
    EncodeReloc *   m_next;
    RelocType       m_relocType;
    BYTE *          m_consumerOffset;  // offset in instruction stream
    IR::Instr *     m_relocInstr;
};



///---------------------------------------------------------------------------
///
/// class EncoderMD
///
///---------------------------------------------------------------------------

class EncoderMD
{
public:
    EncoderMD(Func * func) : m_func(func) { }
    ptrdiff_t       Encode(IR::Instr * instr, BYTE *pc, BYTE* beginCodeAddress = nullptr);
    void            Init(Encoder *encoder);
    void            ApplyRelocs(size_t codeBufferAddress, size_t codeSize, uint* bufferCRC, BOOL isBrShorteningSucceeded, bool isFinalBufferValidation = false);
    static bool     TryConstFold(IR::Instr *instr, IR::RegOpnd *regOpnd);
    static bool     TryFold(IR::Instr *instr, IR::RegOpnd *regOpnd);
    const BYTE      GetRegEncode(IR::RegOpnd *regOpnd);
    const BYTE      GetFloatRegEncode(IR::RegOpnd *regOpnd);
    static const BYTE GetRegEncode(RegNum reg);
    static uint32   GetOpdope(IR::Instr *instr);
    static uint32   GetOpdope(Js::OpCode op);

    static bool     IsLoad(IR::Instr *instr)
    {
        return ISLOAD(instr->m_opcode);
    }

    static bool     IsStore(IR::Instr *instr)
    {
        return ISSTORE(instr->m_opcode);
    }

    static bool     IsShifterUpdate(IR::Instr *instr)
    {
        return ISSHIFTERUPDATE(instr->m_opcode);
    }

    static bool     IsShifterSub(IR::Instr *instr)
    {
        return ISSHIFTERSUB(instr->m_opcode);
    }

    static bool     IsShifterPost(IR::Instr *instr)
    {
        return ISSHIFTERPOST(instr->m_opcode);
    }

    static bool     SetsSBit(IR::Instr *instr)
    {
        return SETS_SBIT(instr->m_opcode);
    }

    static DWORD BranchOffset_26(int64 x);
    
    void            AddLabelReloc(BYTE* relocAddress);

    static bool     CanEncodeLogicalConst(IntConstType constant, int size);
    // ToDo (SaAgarwa) Copied from ARM32 to compile. Validate is this correct
    static bool     CanEncodeLoadStoreOffset(int32 offset) { return IS_CONST_UINT12(offset); }
    static void     BaseAndOffsetFromSym(IR::SymOpnd *symOpnd, RegNum *pBaseReg, int32 *pOffset, Func * func);
    void            EncodeInlineeCallInfo(IR::Instr *instr, uint32 offset);
private:
    Func *          m_func;
    Encoder *       m_encoder;
    BYTE *          m_pc;
    EncodeReloc *   m_relocList;
private:
    ULONG           GenerateEncoding(IR::Instr* instr, BYTE *pc, int32 size);
    bool            CanonicalizeInstr(IR::Instr *instr);
    void            CanonicalizeLea(IR::Instr * instr);
    bool            DecodeMemoryOpnd(IR::Opnd* opnd, ARM64_REGISTER &baseRegResult, ARM64_REGISTER &indexRegResult, BYTE &indexScale, int32 &offset);
    
    static bool     EncodeLogicalConst(IntConstType constant, DWORD * result, int size);
};

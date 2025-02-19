//===- llvm/CodeGen/GlobalISel/CallLowering.h - Call lowering ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file describes how to lower LLVM calls to machine code calls.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GLOBALISEL_CALLLOWERING_H
#define LLVM_CODEGEN_GLOBALISEL_CALLLOWERING_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/TargetCallingConv.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MachineValueType.h"
#include <cstdint>
#include <functional>

namespace llvm {

class DataLayout;
class Function;
class MachineIRBuilder;
class MachineOperand;
struct MachinePointerInfo;
class MachineRegisterInfo;
class TargetLowering;
class Type;
class Value;

class CallLowering {
  const TargetLowering *TLI;

  virtual void anchor();
public:
  struct ArgInfo {
    Register Reg;
    Type *Ty;
    ISD::ArgFlagsTy Flags;
    bool IsFixed;

    ArgInfo(unsigned Reg, Type *Ty, ISD::ArgFlagsTy Flags = ISD::ArgFlagsTy{},
            bool IsFixed = true)
      : Reg(Reg), Ty(Ty), Flags(Flags), IsFixed(IsFixed) {
      assert((Ty->isVoidTy() == (Reg == 0)) &&
             "only void types should have no register");
    }
  };

  /// Argument handling is mostly uniform between the four places that
  /// make these decisions: function formal arguments, call
  /// instruction args, call instruction returns and function
  /// returns. However, once a decision has been made on where an
  /// arugment should go, exactly what happens can vary slightly. This
  /// class abstracts the differences.
  struct ValueHandler {
    ValueHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                 CCAssignFn *AssignFn)
      : MIRBuilder(MIRBuilder), MRI(MRI), AssignFn(AssignFn) {}

    virtual ~ValueHandler() = default;

    /// Returns true if the handler is dealing with formal arguments,
    /// not with return values etc.
    virtual bool isArgumentHandler() const { return false; }

    /// Materialize a VReg containing the address of the specified
    /// stack-based object. This is either based on a FrameIndex or
    /// direct SP manipulation, depending on the context. \p MPO
    /// should be initialized to an appropriate description of the
    /// address created.
    virtual Register getStackAddress(uint64_t Size, int64_t Offset,
                                     MachinePointerInfo &MPO) = 0;

    /// The specified value has been assigned to a physical register,
    /// handle the appropriate COPY (either to or from) and mark any
    /// relevant uses/defines as needed.
    virtual void assignValueToReg(Register ValVReg, Register PhysReg,
                                  CCValAssign &VA) = 0;

    /// The specified value has been assigned to a stack
    /// location. Load or store it there, with appropriate extension
    /// if necessary.
    virtual void assignValueToAddress(Register ValVReg, Register Addr,
                                      uint64_t Size, MachinePointerInfo &MPO,
                                      CCValAssign &VA) = 0;

    /// Handle custom values, which may be passed into one or more of \p VAs.
    /// \return The number of \p VAs that have been assigned after the first
    ///         one, and which should therefore be skipped from further
    ///         processing.
    virtual unsigned assignCustomValue(const ArgInfo &Arg,
                                       ArrayRef<CCValAssign> VAs) {
      // This is not a pure virtual method because not all targets need to worry
      // about custom values.
      llvm_unreachable("Custom values not supported");
    }

    Register extendRegister(Register ValReg, CCValAssign &VA);

    virtual bool assignArg(unsigned ValNo, MVT ValVT, MVT LocVT,
                           CCValAssign::LocInfo LocInfo, const ArgInfo &Info,
                           CCState &State) {
      return AssignFn(ValNo, ValVT, LocVT, LocInfo, Info.Flags, State);
    }

    MachineIRBuilder &MIRBuilder;
    MachineRegisterInfo &MRI;
    CCAssignFn *AssignFn;

  private:
    virtual void anchor();
  };

protected:
  /// Getter for generic TargetLowering class.
  const TargetLowering *getTLI() const {
    return TLI;
  }

  /// Getter for target specific TargetLowering class.
  template <class XXXTargetLowering>
    const XXXTargetLowering *getTLI() const {
    return static_cast<const XXXTargetLowering *>(TLI);
  }

  template <typename FuncInfoTy>
  void setArgFlags(ArgInfo &Arg, unsigned OpIdx, const DataLayout &DL,
                   const FuncInfoTy &FuncInfo) const;

  /// Invoke Handler::assignArg on each of the given \p Args and then use
  /// \p Callback to move them to the assigned locations.
  ///
  /// \return True if everything has succeeded, false otherwise.
  bool handleAssignments(MachineIRBuilder &MIRBuilder, ArrayRef<ArgInfo> Args,
                         ValueHandler &Handler) const;

public:
  CallLowering(const TargetLowering *TLI) : TLI(TLI) {}
  virtual ~CallLowering() = default;

  /// \return true if the target is capable of handling swifterror values that
  /// have been promoted to a specified register. The extended versions of
  /// lowerReturn and lowerCall should be implemented.
  virtual bool supportSwiftError() const {
    return false;
  }

  /// This hook must be implemented to lower outgoing return values, described
  /// by \p Val, into the specified virtual registers \p VRegs.
  /// This hook is used by GlobalISel.
  ///
  /// \p SwiftErrorVReg is non-zero if the function has a swifterror parameter
  /// that needs to be implicitly returned.
  ///
  /// \return True if the lowering succeeds, false otherwise.
  virtual bool lowerReturn(MachineIRBuilder &MIRBuilder, const Value *Val,
                           ArrayRef<Register> VRegs,
                           Register SwiftErrorVReg) const {
    if (!supportSwiftError()) {
      assert(SwiftErrorVReg == 0 && "attempt to use unsupported swifterror");
      return lowerReturn(MIRBuilder, Val, VRegs);
    }
    return false;
  }

  /// This hook behaves as the extended lowerReturn function, but for targets
  /// that do not support swifterror value promotion.
  virtual bool lowerReturn(MachineIRBuilder &MIRBuilder, const Value *Val,
                           ArrayRef<Register> VRegs) const {
    return false;
  }


  /// This hook must be implemented to lower the incoming (formal)
  /// arguments, described by \p Args, for GlobalISel. Each argument
  /// must end up in the related virtual register described by VRegs.
  /// In other words, the first argument should end up in VRegs[0],
  /// the second in VRegs[1], and so on.
  /// \p MIRBuilder is set to the proper insertion for the argument
  /// lowering.
  ///
  /// \return True if the lowering succeeded, false otherwise.
  virtual bool lowerFormalArguments(MachineIRBuilder &MIRBuilder,
                                    const Function &F,
                                    ArrayRef<Register> VRegs) const {
    return false;
  }

  /// This hook must be implemented to lower the given call instruction,
  /// including argument and return value marshalling.
  ///
  /// \p CallConv is the calling convention to be used for the call.
  ///
  /// \p Callee is the destination of the call. It should be either a register,
  /// globaladdress, or externalsymbol.
  ///
  /// \p OrigRet is a descriptor for the return type of the function.
  ///
  /// \p OrigArgs is a list of descriptors of the arguments passed to the
  /// function.
  ///
  /// \p SwiftErrorVReg is non-zero if the call has a swifterror inout
  /// parameter, and contains the vreg that the swifterror should be copied into
  /// after the call.
  ///
  /// \return true if the lowering succeeded, false otherwise.
  virtual bool lowerCall(MachineIRBuilder &MIRBuilder, CallingConv::ID CallConv,
                         const MachineOperand &Callee, const ArgInfo &OrigRet,
                         ArrayRef<ArgInfo> OrigArgs,
                         Register SwiftErrorVReg) const {
    if (!supportSwiftError()) {
      assert(SwiftErrorVReg == 0 && "trying to use unsupported swifterror");
      return lowerCall(MIRBuilder, CallConv, Callee, OrigRet, OrigArgs);
    }
    return false;
  }

  /// This hook behaves as the extended lowerCall function, but for targets that
  /// do not support swifterror value promotion.
  virtual bool lowerCall(MachineIRBuilder &MIRBuilder, CallingConv::ID CallConv,
                         const MachineOperand &Callee, const ArgInfo &OrigRet,
                         ArrayRef<ArgInfo> OrigArgs) const {
    return false;
  }

  /// Lower the given call instruction, including argument and return value
  /// marshalling.
  ///
  /// \p CI is the call/invoke instruction.
  ///
  /// \p ResReg is a register where the call's return value should be stored (or
  /// 0 if there is no return value).
  ///
  /// \p ArgRegs is a list of virtual registers containing each argument that
  /// needs to be passed.
  ///
  /// \p SwiftErrorVReg is non-zero if the call has a swifterror inout
  /// parameter, and contains the vreg that the swifterror should be copied into
  /// after the call.
  ///
  /// \p GetCalleeReg is a callback to materialize a register for the callee if
  /// the target determines it cannot jump to the destination based purely on \p
  /// CI. This might be because \p CI is indirect, or because of the limited
  /// range of an immediate jump.
  ///
  /// \return true if the lowering succeeded, false otherwise.
  bool lowerCall(MachineIRBuilder &MIRBuilder, ImmutableCallSite CS,
                 Register ResReg, ArrayRef<Register> ArgRegs,
                 Register SwiftErrorVReg,
                 std::function<unsigned()> GetCalleeReg) const;

};

} // end namespace llvm

#endif // LLVM_CODEGEN_GLOBALISEL_CALLLOWERING_H

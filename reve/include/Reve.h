#ifndef REVE_H
#define REVE_H

#include "PathAnalysis.h"
#include "SMT.h"

#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Driver/Driver.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Option/Option.h"

using Assignment = std::tuple<std::string, SMTRef>;

struct CallInfo {
    std::string AssignedTo;
    std::string CallName;
    std::vector<SMTRef> Args;
    CallInfo(std::string AssignedTo, std::string CallName,
             std::vector<SMTRef> Args)
        : AssignedTo(AssignedTo), CallName(CallName), Args(Args) {}
};

enum DefOrCallInfoTag { Call, Def };

struct DefOrCallInfo {
    std::shared_ptr<Assignment> Definition;
    std::shared_ptr<CallInfo> CallInfo;
    enum DefOrCallInfoTag Tag;
    DefOrCallInfo(std::shared_ptr<Assignment> Definition)
        : Definition(Definition), CallInfo(nullptr), Tag(Def) {}
    DefOrCallInfo(std::shared_ptr<struct CallInfo> CallInfo)
        : Definition(nullptr), CallInfo(CallInfo), Tag(Call) {}
};

struct Assignments {
    std::vector<DefOrCallInfo> Definitions;
    SMTRef Condition;
    Assignments(std::vector<DefOrCallInfo> Definitions, SMTRef Condition)
        : Definitions(Definitions), Condition(Condition) {}
};

struct CleanAssignments {
    std::vector<Assignment> Definitions;
    SMTRef Condition;
    CleanAssignments(std::vector<Assignment> Definitions, SMTRef Condition)
        : Definitions(Definitions), Condition(Condition) {}
};

auto splitAssignments(std::vector<Assignments>)
    -> std::pair<std::vector<std::vector<CleanAssignments>>,
                 std::vector<CallInfo>>;
auto toCallInfo(std::string AssignedTo, const llvm::CallInst *CallInst)
    -> std::shared_ptr<CallInfo>;
auto main(int Argc, const char **Argv) -> int;
auto getFunction(llvm::Module &Mod) -> llvm::ErrorOr<llvm::Function &>;
template <int N>
auto initializeArgs(const char *ExeName, std::string Input1, std::string Input2)
    -> llvm::SmallVector<const char *, N>;
auto initializeDiagnostics(void) -> std::unique_ptr<clang::DiagnosticsEngine>;
auto initializeDriver(clang::DiagnosticsEngine &Diags)
    -> std::unique_ptr<clang::driver::Driver>;
auto preprocessModule(llvm::Function &Fun, std::string Prefix)
    -> std::unique_ptr<llvm::FunctionAnalysisManager>;
auto getCmd(clang::driver::Compilation &Comp, clang::DiagnosticsEngine &Diags)
    -> llvm::ErrorOr<
        std::tuple<llvm::opt::ArgStringList, llvm::opt::ArgStringList>>;
template <typename T> auto makeErrorOr(T Arg) -> llvm::ErrorOr<T>;
auto getModule(const char *ExeName, std::string Input1, std::string Input2)
    -> std::tuple<std::unique_ptr<clang::CodeGenAction>,
                  std::unique_ptr<clang::CodeGenAction>>;
auto getCodeGenAction(const llvm::opt::ArgStringList &CCArgs,
                      clang::DiagnosticsEngine &Diags)
    -> std::unique_ptr<clang::CodeGenAction>;
auto convertToSMT(llvm::Function &Fun1, llvm::Function &Fun2,
                  std::unique_ptr<llvm::FunctionAnalysisManager> Fam1,
                  std::unique_ptr<llvm::FunctionAnalysisManager> Fam2) -> void;
auto toDef(const llvm::Instruction &Instr, const llvm::BasicBlock *PrevBB,
           set<string> &Constructed)
    -> std::shared_ptr<std::tuple<std::string, SMTRef>>;
auto getPredName(const llvm::CmpInst::Predicate Pred) -> std::string;
auto getInstrNameOrValue(const llvm::Value *Val, const llvm::Type *Ty,
                         std::set<std::string> &Constructed) -> SMTRef;
auto invariant(int StartIndex, int EndIndex, std::set<std::string> InputArgs,
               std::set<std::string> EndArgs) -> SMTRef;
auto getOpName(const llvm::BinaryOperator &Op) -> std::string;
auto swapIndex(int I) -> int;
auto instrToDefs(const llvm::BasicBlock *BB, const llvm::BasicBlock *PrevBB,
                 bool IgnorePhis, bool OnlyPhis, int Program,
                 set<string> &Constructed) -> std::vector<DefOrCallInfo>;
auto pathToSMT(Path Path, int Program, std::set<std::string> FreeVars,
               bool ToEnd) -> std::vector<Assignments>;
auto invName(int Index) -> std::string;
auto wrapForall(SMTRef Clause, std::set<std::string> FreeVars) -> SMTRef;
auto invariantDef(int BlockIndex, std::set<std::string> FreeVars) -> SMTRef;
auto freeVars(std::map<int, Paths> PathMap)
    -> std::pair<std::set<std::string>, std::map<int, std::set<std::string>>>;
auto freeVarsMap(PathMap Map1, PathMap Map2, set<string> FunArgs)
    -> std::map<int, std::set<std::string>>;
auto functionArgs(llvm::Function &Fun1, llvm::Function &Fun2)
    -> std::pair<std::vector<std::string>, std::vector<std::string>>;
auto wrapToplevelForall(SMTRef Clause, std::set<std::string> Args) -> SMTRef;
auto makeFunArgsEqual(SMTRef Clause, std::set<std::string> Args1,
                      std::set<std::string> Args2) -> SMTRef;
auto forbiddenPaths(PathMap PathMap1, PathMap PathMap2,
                    std::map<int, set<string>> FreeVarsMap)
    -> std::vector<SMTRef>;
auto synchronizedPaths(PathMap PathMap1, PathMap PathMap2,
                       std::map<int, set<string>> FreeVarsMap,
                       std::vector<string> FunArgs1,
                       std::vector<string> FunArgs2)
    -> std::pair<std::vector<SMTRef>, std::vector<SMTRef>>;
auto termInv(std::set<std::string> FunArgs) -> SMTRef;
auto equalInputsEqualOutputs(set<string> FunArgs, std::vector<string> FunArgs1,
                             std::vector<string> FunArgs2) -> SMTRef;
auto filterVars(int Program, std::set<std::string> Vars)
    -> std::set<std::string>;
auto interleaveSMT(SMTRef EndClause, std::vector<Assignments> Assignments1,
                   std::vector<Assignments> Assignments2,
                   std::vector<string> FunArgs1, std::vector<string> FunArgs2)
    -> SMTRef;
auto recursiveForall(SMTRef Clause, std::vector<SMTRef> Args1,
                     std::vector<SMTRef> Args2, std::string Ret1,
                     std::string Ret2, std::vector<string> FunArgs1,
                     std::vector<string> FunArgs2) -> SMTRef;

#endif // REVE_H

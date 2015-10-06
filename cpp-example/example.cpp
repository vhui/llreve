#include <iostream>
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Tool.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "example.h"

using clang::CodeGenAction;
using clang::CompilerInstance;
using clang::CompilerInvocation;
using clang::DiagnosticsEngine;

using clang::driver::ArgStringList;
using clang::driver::Command;
using clang::driver::Compilation;
using clang::driver::Driver;
using clang::driver::JobList;

using llvm::ErrorOr;
using llvm::errs;
using llvm::IntrusiveRefCntPtr;

using std::unique_ptr;

template <int N>
llvm::SmallVector<const char *, N> initializeArgs(const char **argv, int argc) {
    llvm::SmallVector<const char *, N> args;
    args.push_back(argv[0]);            // add executable name
    args.push_back("-xc");              // force language to C
    args.append(argv + 1, argv + argc); // add remaining args
    args.push_back("-fsyntax-only");    // don't do more work than necessary
    return args;
}

unique_ptr<DiagnosticsEngine> initializeDiagnostics() {
    const IntrusiveRefCntPtr<clang::DiagnosticOptions> diagOpts =
        new clang::DiagnosticOptions();
    auto diagClient =
        new clang::TextDiagnosticPrinter(llvm::errs(), &*diagOpts);
    const IntrusiveRefCntPtr<clang::DiagnosticIDs> diagID(
        new clang::DiagnosticIDs());
    return std::make_unique<DiagnosticsEngine>(diagID, &*diagOpts, diagClient);
}

unique_ptr<Driver> initializeDriver(DiagnosticsEngine &Diags) {
    std::string tripleStr = llvm::sys::getProcessTriple();
    llvm::Triple triple(tripleStr);
    auto driver =
        std::make_unique<Driver>("/usr/bin/clang", triple.str(), Diags);
    driver->setTitle("clang/llvm example");
    driver->setCheckInputsExist(false);
    return driver;
}

ErrorOr<const Command &> getCmd(Compilation &Comp, DiagnosticsEngine &Diags) {
    const JobList &jobs = Comp.getJobs();

    // there should be only one job
    if (jobs.size() != 1) {
        llvm::SmallString<256> Msg;
        llvm::raw_svector_ostream OS(Msg);
        jobs.Print(OS, "; ", true);
        Diags.Report(clang::diag::err_fe_expected_compiler_job) << OS.str();
        return ErrorOr<const Command &>(std::error_code());
    }

    const Command &Cmd = llvm::cast<Command>(*jobs.begin());
    if (StringRef(Cmd.getCreator().getName()) != "clang") {
        Diags.Report(clang::diag::err_fe_expected_clang_command);
        return ErrorOr<const Command &>(std::error_code());
    }
    return makeErrorOr(Cmd);
}

template <typename T> ErrorOr<T> makeErrorOr(T Arg) { return ErrorOr<T>(Arg); }

unique_ptr<clang::CodeGenAction> getModule(int argc, const char **argv) {
    auto diags = initializeDiagnostics();
    auto driver = initializeDriver(*diags);
    auto args = initializeArgs<16>(argv, argc);

    std::unique_ptr<Compilation> comp(driver->BuildCompilation(args));
    if (!comp) {
        return nullptr;
    }

    auto CmdOrError = getCmd(*comp, *diags);
    if (!CmdOrError) {
        return nullptr;
    }
    auto Cmd = CmdOrError.get();

    const ArgStringList &CCArgs = Cmd.getArguments();
    std::unique_ptr<CompilerInvocation> CI(new CompilerInvocation);
    CompilerInvocation::CreateFromArgs(
        *CI, const_cast<const char **>(CCArgs.data()),
        const_cast<const char **>(CCArgs.data()) + CCArgs.size(), *diags);

    // Create a compiler instance to handle the actual work.
    CompilerInstance Clang;
    Clang.setInvocation(CI.release());

    // Create the compilers actual diagnostics engine.
    Clang.createDiagnostics();
    if (!Clang.hasDiagnostics()) {
        std::cout << "Couldn't enable diagnostics\n";
        return nullptr;
    }

    std::unique_ptr<CodeGenAction> Act(new clang::EmitLLVMOnlyAction());

    if (!Clang.ExecuteAction(*Act)) {
        std::cout << "Couldn't execute action\n";
        return nullptr;
    }
    return Act;
}

int main(int argc, const char **argv) {
    auto Act = getModule(argc, argv);
    if (!Act) {
        return 1;
    }

    auto Mod = Act->takeModule();
    if (!Mod) {
        return 1;
    }

    ErrorOr<llvm::Function &> funOrError = getFunction(*Mod);
    if (!funOrError) {
        errs() << "Couldn't find a function\n";
        return 1;
    }

    doAnalysis(funOrError.get());

    llvm::llvm_shutdown();

    return 0;
}

void doAnalysis(llvm::Function &fun) {
    llvm::FunctionAnalysisManager fam(true); // enable debug log
    fam.registerPass(llvm::DominatorTreeAnalysis());
    fam.registerPass(llvm::LoopAnalysis());

    llvm::FunctionPassManager fpm(true); // enable debug log

    fpm.addPass(llvm::PrintFunctionPass(errs())); // dump function
    auto results = fpm.run(fun, &fam);

    fam.getResult<llvm::LoopAnalysis>(fun).print(errs());
}

ErrorOr<llvm::Function &> getFunction(llvm::Module &mod) {
    if (mod.getFunctionList().size() == 0) {
        return ErrorOr<llvm::Function &>(std::error_code());
    }
    llvm::Function &fun = mod.getFunctionList().front();
    if (mod.getFunctionList().size() > 1) {
        llvm::errs() << "Warning: There is more than one function in the "
                        "module, choosing “"
                     << fun.getName() << "”\n";
    }
    return ErrorOr<llvm::Function &>(fun);
}

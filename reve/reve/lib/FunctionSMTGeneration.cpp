/*
 * This file is part of
 *    llreve - Automatic regression verification for LLVM programs
 *
 * Copyright (C) 2016 Karlsruhe Institute of Technology
 *
 * The system is published under a BSD license.
 * See LICENSE (distributed with this file) for details.
 */

#include "FunctionSMTGeneration.h"

#include "Compat.h"
#include "Invariant.h"
#include "ModuleSMTGeneration.h"
#include "Opts.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Operator.h"

#include <iostream>

using llvm::CmpInst;
using smt::Assert;
using smt::Assignment;
using smt::Comment;
using smt::Forall;
using smt::FunDef;
using smt::Op;
using smt::SMTExpr;
using smt::SMTRef;
using smt::SharedSMTRef;
using smt::SortedVar;
using smt::VarDecl;
using smt::makeOp;
using smt::stringExpr;
using std::function;
using std::make_pair;
using std::make_shared;
using std::map;
using std::set;
using std::string;
using std::string;
using std::unique_ptr;
using std::vector;

vector<SharedSMTRef>
functionAssertion(MonoPair<PreprocessedFunction> preprocessedFuns,
                  vector<SharedSMTRef> &declarations,
                  vector<SortedVar> &variableDeclarations) {
    const MonoPair<PathMap> pathMaps = preprocessedFuns.map<PathMap>(
        [](PreprocessedFunction fun) { return fun.results.paths; });
    checkPathMaps(pathMaps.first, pathMaps.second);
    const MonoPair<BidirBlockMarkMap> marked =
        preprocessedFuns.map<BidirBlockMarkMap>(
            [](PreprocessedFunction fun) { return fun.results.blockMarkMap; });
    const string funName = preprocessedFuns.first.fun->getName().str() + "^" +
                           preprocessedFuns.second.fun->getName().str();
    const MonoPair<vector<smt::SortedVar>> funArgsPair =
        functionArgs(*preprocessedFuns.first.fun, *preprocessedFuns.second.fun);
    const vector<smt::SortedVar> funArgs = functionArgsFreeVars(
        *preprocessedFuns.first.fun, *preprocessedFuns.second.fun);
    const smt::FreeVarsMap freeVarsMap =
        freeVars(pathMaps.first, pathMaps.second, funArgs);
    if (SMTGenerationOpts::getInstance().MuZ) {
        declareVariables(freeVarsMap, variableDeclarations);
    }
    vector<SharedSMTRef> smtExprs;
    vector<SharedSMTRef> pathExprs;

    const auto addInvariant = [&](SMTRef invariant) {
        declarations.push_back(std::move(invariant));

    };
    const llvm::Type *returnType = preprocessedFuns.first.fun->getReturnType();
    MonoPair<SMTRef> invariants =
        invariantDeclaration(ENTRY_MARK, freeVarsMap.at(ENTRY_MARK),
                             ProgramSelection::Both, funName, returnType);
    MonoPair<SMTRef> invariants1 = invariantDeclaration(
        ENTRY_MARK, filterVars(1, freeVarsMap.at(ENTRY_MARK)),
        ProgramSelection::First, preprocessedFuns.first.fun->getName(),
        returnType);
    MonoPair<SMTRef> invariants2 = invariantDeclaration(
        ENTRY_MARK, filterVars(2, freeVarsMap.at(ENTRY_MARK)),
        ProgramSelection::Second, preprocessedFuns.second.fun->getName(),
        returnType);
    std::move(invariants).forEach(addInvariant);
    std::move(invariants1).forEach(addInvariant);
    std::move(invariants2).forEach(addInvariant);

    const map<int, map<int, vector<std::function<SharedSMTRef(SharedSMTRef)>>>>
        synchronizedPaths = getSynchronizedPaths(
            pathMaps.first, pathMaps.second, variableDeclarations, freeVarsMap);
    const vector<SharedSMTRef> recDecls =
        recDeclarations(pathMaps.first, funName, freeVarsMap, returnType);
    declarations.insert(declarations.end(), recDecls.begin(), recDecls.end());
    for (const auto &it : synchronizedPaths) {
        const int startIndex = it.first;
        for (const auto &it2 : it.second) {
            const int endIndex = it2.first;
            const SharedSMTRef endInvariant =
                invariant(startIndex, endIndex, freeVarsMap.at(startIndex),
                          freeVarsMap.at(endIndex), ProgramSelection::Both,
                          funName, freeVarsMap);
            for (const auto &pathFun : it2.second) {
                pathExprs.push_back(make_shared<Assert>(forallStartingAt(
                    pathFun(endInvariant), freeVarsMap.at(startIndex),
                    startIndex, ProgramSelection::Both, funName, false,
                    freeVarsMap)));
            }
        }
    }
    // these are needed for calls that can’t be matched with the same call in
    // the other program
    nonmutualPaths(pathMaps.first, pathExprs, freeVarsMap, Program::First,
                   preprocessedFuns.first.fun->getName(), declarations,
                   variableDeclarations, returnType);
    nonmutualPaths(pathMaps.second, pathExprs, freeVarsMap, Program::Second,
                   preprocessedFuns.second.fun->getName(), declarations,
                   variableDeclarations, returnType);

    const vector<SharedSMTRef> forbiddenPaths = getForbiddenPaths(
        pathMaps, marked, variableDeclarations, freeVarsMap, funName, false);
    pathExprs.insert(pathExprs.end(), forbiddenPaths.begin(),
                     forbiddenPaths.end());

    if (!SMTGenerationOpts::getInstance().PerfectSync) {
        const map<int,
                  map<int, vector<std::function<SharedSMTRef(SharedSMTRef)>>>>
            offByNPaths =
                getOffByNPaths(pathMaps.first, pathMaps.second, freeVarsMap,
                               variableDeclarations, funName, false);
        for (const auto &it : offByNPaths) {
            const int startIndex = it.first;
            for (const auto &it2 : it.second) {
                for (const auto &pathFun : it2.second) {
                    pathExprs.push_back(make_shared<Assert>(forallStartingAt(
                        pathFun(nullptr), freeVarsMap.at(startIndex),
                        startIndex, ProgramSelection::Both, funName, false,
                        freeVarsMap)));
                }
            }
        }
    }

    smtExprs.insert(smtExprs.end(), pathExprs.begin(), pathExprs.end());

    return smtExprs;
}

vector<SharedSMTRef> slicingAssertion(MonoPair<PreprocessedFunction> funPair) {
    vector<SharedSMTRef> assertions;

    string typeBool = "Bool";
    string typeInt = "Int";
    std::string name;

    // Collect arguments for call
    std::vector<SortedVar> args;

    auto funArgs1 = funArgs(*(funPair.first.fun), "arg1_", 0);
    auto funArgs2 = funArgs(*(funPair.second.fun), "arg2_", 0);
    assert(funArgs1.size() == funArgs2.size());

    args.insert(args.end(), funArgs1.begin(), funArgs1.end());
    args.insert(args.end(), funArgs2.begin(), funArgs2.end());

    // Set preconditition for nonmutual calls to false. This ensures
    // that only mutual calls are allowed.
    // Note that we assume the number of arguments to be equal (number of
    // variables in the criterion). This is why we can use the same arg names.
    makeMonoPair(funArgs1, funArgs2)
        .indexedForEachProgram([&assertions, typeBool](auto funArgs,
                                                       auto program) {
            std::string invName =
                invariantName(ENTRY_MARK, asSelection(program), "__criterion",
                              InvariantAttr::PRE, 0);
            assertions.push_back(make_shared<FunDef>(invName, funArgs, typeBool,
                                                     stringExpr("false")));
        });

    // Ensure all variables in the criterion are equal in both versions
    // of the program using the mutual precondition
    vector<SharedSMTRef> equalArgs;
    for (auto argPair : makeZip(funArgs1, funArgs2)) {
        equalArgs.push_back(
            makeOp("=", argPair.first.name, argPair.second.name));
    }

    SharedSMTRef allEqual = make_shared<Op>("and", equalArgs);
    name = invariantName(ENTRY_MARK, ProgramSelection::Both, "__criterion",
                         InvariantAttr::PRE, 0);
    assertions.push_back(make_shared<FunDef>(name, args, typeBool, allEqual));

    // Finaly we need to set the invariant for the function itself to true.
    // (The __criterion function does nothing, therefore no restrictions arise)
    // This is true for mutual and non mutual calls.
    args.push_back(SortedVar("ret1", typeInt));
    args.push_back(SortedVar("ret2", typeInt));

    SharedSMTRef invBody = stringExpr("true");
    name = invariantName(ENTRY_MARK, ProgramSelection::Both, "__criterion",
                         InvariantAttr::NONE, 0);
    assertions.push_back(make_shared<FunDef>(name, args, typeBool, invBody));

    makeMonoPair(funArgs1, funArgs2)
        .indexedForEachProgram([&assertions, &invBody, typeInt,
                                typeBool](auto funArgs, auto program) {
            funArgs.push_back(SortedVar(
                "ret" + std::to_string(programIndex(program)), typeInt));
            std::string invName =
                invariantName(ENTRY_MARK, asSelection(program), "__criterion",
                              InvariantAttr::NONE, 0);
            assertions.push_back(
                make_shared<FunDef>(invName, funArgs, typeBool, invBody));
        });

    return assertions;
}

// the main function that we want to check doesn’t need the output parameters in
// the assertions since it is never called
vector<SharedSMTRef>
mainAssertion(MonoPair<PreprocessedFunction> preprocessedFuns,
              vector<SharedSMTRef> &declarations,
              vector<SortedVar> &variableDeclarations, bool onlyRec) {
    const MonoPair<PathMap> pathMaps = preprocessedFuns.map<PathMap>(
        [](PreprocessedFunction fun) { return fun.results.paths; });
    checkPathMaps(pathMaps.first, pathMaps.second);
    const MonoPair<BidirBlockMarkMap> marked =
        preprocessedFuns.map<BidirBlockMarkMap>(
            [](PreprocessedFunction fun) { return fun.results.blockMarkMap; });
    const string funName = preprocessedFuns.first.fun->getName();
    const MonoPair<vector<smt::SortedVar>> funArgsPair =
        functionArgs(*preprocessedFuns.first.fun, *preprocessedFuns.second.fun);
    const vector<smt::SortedVar> funArgs = functionArgsFreeVars(
        *preprocessedFuns.first.fun, *preprocessedFuns.second.fun);

    const smt::FreeVarsMap freeVarsMap =
        freeVars(pathMaps.first, pathMaps.second, funArgs);
    if (SMTGenerationOpts::getInstance().MuZ ||
        SMTGenerationOpts::getInstance().Invert) {
        declareVariables(freeVarsMap, variableDeclarations);
    }
    vector<SharedSMTRef> smtExprs;

    const llvm::Type *returnType = preprocessedFuns.first.fun->getReturnType();
    if (onlyRec) {
        smtExprs.push_back(
            equalInputsEqualOutputs(freeVarsMap.at(ENTRY_MARK),
                                    filterVars(1, freeVarsMap.at(ENTRY_MARK)),
                                    filterVars(2, freeVarsMap.at(ENTRY_MARK)),
                                    funName, freeVarsMap, returnType));
        return smtExprs;
    }

    map<int, map<int, vector<std::function<SharedSMTRef(SharedSMTRef)>>>>
        synchronizedPaths = getSynchronizedPaths(
            pathMaps.first, pathMaps.second, variableDeclarations, freeVarsMap);
    const vector<SharedSMTRef> mainDecls =
        mainDeclarations(pathMaps.first, funName, freeVarsMap);
    declarations.insert(declarations.end(), mainDecls.begin(), mainDecls.end());
    const vector<SharedSMTRef> forbiddenPaths = getForbiddenPaths(
        pathMaps, marked, variableDeclarations, freeVarsMap, funName, true);
    if (!SMTGenerationOpts::getInstance().PerfectSync) {
        const map<int,
                  map<int, vector<std::function<SharedSMTRef(SharedSMTRef)>>>>
            offByNPaths =
                getOffByNPaths(pathMaps.first, pathMaps.second, freeVarsMap,
                               variableDeclarations, funName, true);
        synchronizedPaths = mergePathFuns(synchronizedPaths, offByNPaths);
    }
    auto transposedPaths = transpose(synchronizedPaths);
    // remove cycles
    for (auto &it : transposedPaths) {
        it.second.erase(it.first);
    }
    vector<SharedSMTRef> negations;
    for (const auto &it : synchronizedPaths) {
        const int startIndex = it.first;
        vector<SharedSMTRef> pathsStartingHere;
        for (auto it2 : it.second) {
            const int endIndex = it2.first;
            SharedSMTRef endInvariant =
                mainInvariant(endIndex, freeVarsMap.at(endIndex), funName);
            if (SMTGenerationOpts::getInstance().MuZ && endIndex == EXIT_MARK) {
                endInvariant =
                    makeOp("=>", makeOp("not", std::move(endInvariant)),
                           stringExpr("END_QUERY"));
            }
            for (auto pathFun : it2.second) {
                pathsStartingHere.push_back(pathFun(endInvariant));
            }
        }
        if (SMTGenerationOpts::getInstance().Nest) {
            auto clause = forallStartingAt(
                make_shared<Op>("and", pathsStartingHere),
                freeVarsMap.at(startIndex), startIndex, ProgramSelection::Both,
                funName, true, freeVarsMap);
            if ((transposedPaths.find(startIndex) != transposedPaths.end())) {
                if (transposedPaths.at(startIndex).size() == 1) {
                    auto it = transposedPaths.at(startIndex).begin();
                    const int comingFrom = it->first;
                    if (it->second.size() == 1) {
                        const auto nestFun = it->second.at(0);
                        clause = forallStartingAt(
                            nestFun(clause), freeVarsMap.at(comingFrom),
                            comingFrom, ProgramSelection::Both, funName, true,
                            freeVarsMap);
                    }
                }
            }
            smtExprs.push_back(make_shared<Assert>(clause));
        } else {
            for (auto path : pathsStartingHere) {
                auto clause = forallStartingAt(
                    path, freeVarsMap.at(startIndex), startIndex,
                    ProgramSelection::Both, funName, true, freeVarsMap);
                if (SMTGenerationOpts::getInstance().Invert) {
                    negations.push_back(makeOp(
                        "and",
                        makeOp("=", "INV_INDEX", std::to_string(startIndex)),
                        makeOp("not", clause)));
                } else {
                    smtExprs.push_back(make_shared<Assert>(clause));
                }
            }
        }
    }
    if (SMTGenerationOpts::getInstance().Invert) {
        smtExprs.push_back(
            make_shared<Assert>(make_shared<Op>("or", negations)));
    }
    smtExprs.insert(smtExprs.end(), forbiddenPaths.begin(),
                    forbiddenPaths.end());
    return smtExprs;
}

/* -------------------------------------------------------------------------- */
// Generate SMT for all paths

map<int, map<int, vector<function<SharedSMTRef(SharedSMTRef)>>>>
getSynchronizedPaths(PathMap pathMap1, PathMap pathMap2,
                     vector<SortedVar> &variableDeclarations,
                     smt::FreeVarsMap freeVarsMap) {
    map<int, map<int, vector<function<SharedSMTRef(SharedSMTRef)>>>> pathFuns;
    for (const auto &pathMapIt : pathMap1) {
        const int startIndex = pathMapIt.first;
        for (const auto &innerPathMapIt : pathMapIt.second) {
            const int endIndex = innerPathMapIt.first;
            if (pathMap2.at(startIndex).find(endIndex) !=
                pathMap2.at(startIndex).end()) {
                const auto paths = pathMap2.at(startIndex).at(endIndex);
                for (const auto &path1 : innerPathMapIt.second) {
                    for (const auto &path2 : paths) {
                        auto defs =
                            makeMonoPair(make_pair(path1, Program::First),
                                         make_pair(path2, Program::Second))
                                .map<vector<AssignmentCallBlock>>(
                                    [=](std::pair<Path, Program> pair) {
                                        return assignmentsOnPath(
                                            pair.first, pair.second,
                                            freeVarsMap.at(startIndex),
                                            endIndex == EXIT_MARK);
                                    });
                        pathFuns[startIndex][endIndex].push_back(
                            [&variableDeclarations, defs](SharedSMTRef end) {
                                return interleaveAssignments(
                                    end, defs, variableDeclarations);
                            });
                    }
                }
            }
        }
    }

    return pathFuns;
}

vector<SharedSMTRef> mainDeclarations(PathMap pathMap, string funName,
                                      smt::FreeVarsMap freeVarsMap) {
    vector<SharedSMTRef> declarations;
    for (const auto &pathMapIt : pathMap) {
        const int startIndex = pathMapIt.first;
        if (startIndex != ENTRY_MARK) {
            // ignore entry node
            if (SMTGenerationOpts::getInstance().Invariants.find(startIndex) ==
                SMTGenerationOpts::getInstance().Invariants.end()) {
                const auto invariant = mainInvariantDeclaration(
                    startIndex, freeVarsMap.at(startIndex),
                    ProgramSelection::Both, funName);
                declarations.push_back(invariant);
            } else {
                declarations.push_back(
                    SMTGenerationOpts::getInstance().Invariants.at(startIndex));
            }
        }
    }
    return declarations;
}

vector<SharedSMTRef> recDeclarations(PathMap pathMap, string funName,
                                     smt::FreeVarsMap freeVarsMap,
                                     const llvm::Type *returnType) {
    vector<SharedSMTRef> declarations;
    for (const auto &pathMapIt : pathMap) {
        const int startIndex = pathMapIt.first;
        if (startIndex != ENTRY_MARK) {
            // ignore entry node
            MonoPair<SharedSMTRef> invariants = invariantDeclaration(
                startIndex, freeVarsMap.at(startIndex), ProgramSelection::Both,
                funName, returnType);
            declarations.push_back(invariants.first);
            declarations.push_back(invariants.second);
        }
    }
    return declarations;
}

vector<SharedSMTRef> getForbiddenPaths(MonoPair<PathMap> pathMaps,
                                       MonoPair<BidirBlockMarkMap> marked,
                                       vector<SortedVar> &variableDeclarations,
                                       smt::FreeVarsMap freeVarsMap,
                                       string funName, bool main) {
    vector<SharedSMTRef> pathExprs;
    for (const auto &pathMapIt : pathMaps.first) {
        const int startIndex = pathMapIt.first;
        for (const auto &innerPathMapIt1 : pathMapIt.second) {
            const int endIndex1 = innerPathMapIt1.first;
            for (auto &innerPathMapIt2 : pathMaps.second.at(startIndex)) {
                const auto endIndex2 = innerPathMapIt2.first;
                if (endIndex1 != endIndex2) {
                    for (const auto &path1 : innerPathMapIt1.second) {
                        for (const auto &path2 : innerPathMapIt2.second) {
                            const auto endBlocks =
                                makeMonoPair(path1, path2)
                                    .map<llvm::BasicBlock *>(lastBlock);
                            const auto endIndices =
                                zipWith<BidirBlockMarkMap, llvm::BasicBlock *,
                                        set<int>>(
                                    marked, endBlocks,
                                    [](BidirBlockMarkMap marks,
                                       llvm::BasicBlock *endBlock) -> set<int> {
                                        return marks.BlockToMarksMap[endBlock];
                                    });
                            if (SMTGenerationOpts::getInstance().PerfectSync ||
                                ((startIndex != endIndex1 && // no circles
                                  startIndex != endIndex2) &&
                                 intersection(endIndices.first,
                                              endIndices.second)
                                     .empty())) {
                                const auto smt2 = assignmentsOnPath(
                                    path2, Program::Second,
                                    freeVarsMap.at(startIndex),
                                    endIndex2 == EXIT_MARK);
                                const auto smt1 = assignmentsOnPath(
                                    path1, Program::First,
                                    freeVarsMap.at(startIndex),
                                    endIndex1 == EXIT_MARK);
                                // We need to interleave here, because otherwise
                                // extern functions are not matched
                                const auto smt = interleaveAssignments(
                                    stringExpr("false"),
                                    makeMonoPair(smt1, smt2),
                                    variableDeclarations);
                                pathExprs.push_back(
                                    make_shared<Assert>(forallStartingAt(
                                        smt, freeVarsMap.at(startIndex),
                                        startIndex, ProgramSelection::Both,
                                        funName, main, freeVarsMap)));
                            }
                        }
                    }
                }
            }
        }
    }
    return pathExprs;
}

void nonmutualPaths(PathMap pathMap, vector<SharedSMTRef> &pathExprs,
                    smt::FreeVarsMap freeVarsMap, Program prog, string funName,
                    vector<SharedSMTRef> &declarations,
                    vector<SortedVar> &variableDeclarations,
                    const llvm::Type *returnType) {
    const int progIndex = programIndex(prog);
    for (const auto &pathMapIt : pathMap) {
        const int startIndex = pathMapIt.first;
        if (startIndex != ENTRY_MARK) {
            MonoPair<SMTRef> invariants = invariantDeclaration(
                startIndex, filterVars(progIndex, freeVarsMap.at(startIndex)),
                asSelection(prog), funName, returnType);
            std::move(invariants).forEach([&declarations](SMTRef inv) {
                declarations.push_back(std::move(inv));
            });
        }
        for (const auto &innerPathMapIt : pathMapIt.second) {
            const int endIndex = innerPathMapIt.first;
            for (const auto &path : innerPathMapIt.second) {
                SMTRef endInvariant1 =
                    invariant(startIndex, endIndex, freeVarsMap.at(startIndex),
                              freeVarsMap.at(endIndex), asSelection(prog),
                              funName, freeVarsMap);
                const auto defs =
                    assignmentsOnPath(path, prog, freeVarsMap.at(startIndex),
                                      endIndex == EXIT_MARK);
                pathExprs.push_back(make_shared<Assert>(forallStartingAt(
                    nonmutualSMT(std::move(endInvariant1), defs,
                                 variableDeclarations, prog),
                    filterVars(progIndex, freeVarsMap.at(startIndex)),
                    startIndex, asSelection(prog), funName, false,
                    freeVarsMap)));
            }
        }
    }
}

map<int, map<int, vector<function<SharedSMTRef(SharedSMTRef)>>>>
getOffByNPaths(PathMap pathMap1, PathMap pathMap2, smt::FreeVarsMap freeVarsMap,
               vector<SortedVar> &variableDeclarations, string funName,
               bool main) {
    map<int, map<int, vector<function<SharedSMTRef(SharedSMTRef)>>>> pathFuns;
    vector<SharedSMTRef> paths;
    const auto firstPaths =
        offByNPathsOneDir(pathMap1, pathMap2, freeVarsMap, variableDeclarations,
                          Program::First, funName, main);
    const auto secondPaths =
        offByNPathsOneDir(pathMap2, pathMap1, freeVarsMap, variableDeclarations,
                          Program::Second, funName, main);
    return mergePathFuns(firstPaths, secondPaths);
}

map<int, map<int, vector<function<SharedSMTRef(SharedSMTRef)>>>>
offByNPathsOneDir(PathMap pathMap, PathMap otherPathMap,
                  smt::FreeVarsMap freeVarsMap,
                  vector<SortedVar> &variableDeclarations, Program prog,
                  string funName, bool main) {
    const int progIndex = programIndex(prog);
    map<int, map<int, vector<function<SharedSMTRef(SharedSMTRef)>>>> pathFuns;
    for (const auto &pathMapIt : pathMap) {
        const int startIndex = pathMapIt.first;
        for (const auto &innerPathMapIt : pathMapIt.second) {
            const int endIndex = innerPathMapIt.first;
            if (startIndex == endIndex) {
                // we found a loop
                for (const auto &path : innerPathMapIt.second) {
                    const auto endArgs2 = filterVars(
                        swapIndex(progIndex), freeVarsMap.at(startIndex));
                    const auto endArgs =
                        filterVars(progIndex, freeVarsMap.at(startIndex));
                    vector<smt::SortedVar> args;
                    if (prog == Program::First) {
                        for (auto arg : endArgs) {
                            args.push_back(arg);
                        }
                        for (auto arg : endArgs2) {
                            args.push_back(
                                SortedVar(arg.name + "_old", arg.type));
                        }

                    } else {
                        for (auto arg : endArgs2) {
                            args.push_back(
                                SortedVar(arg.name + "_old", arg.type));
                        }
                        for (auto arg : endArgs) {
                            args.push_back(arg);
                        }
                    }
                    SMTRef endInvariant;
                    if (main) {
                        endInvariant = mainInvariant(startIndex, args, funName);
                    } else {
                        endInvariant = invariant(
                            startIndex, startIndex, freeVarsMap.at(startIndex),
                            args, ProgramSelection::Both, funName, freeVarsMap);
                    }
                    SharedSMTRef dontLoopInvariant = getDontLoopInvariant(
                        std::move(endInvariant), startIndex, otherPathMap,
                        freeVarsMap, variableDeclarations, swapProgram(prog));
                    const auto defs =
                        assignmentsOnPath(path, prog, freeVarsMap.at(endIndex),
                                          endIndex == EXIT_MARK);
                    pathFuns[startIndex][startIndex].push_back(
                        [&variableDeclarations, defs, prog,
                         dontLoopInvariant](SharedSMTRef /*unused*/) {
                            return nonmutualSMT(dontLoopInvariant, defs,
                                                variableDeclarations, prog);
                        });
                }
            }
        }
    }
    return pathFuns;
}

/* -------------------------------------------------------------------------- */
// Functions for generating SMT for a single/mutual path

vector<AssignmentCallBlock> assignmentsOnPath(Path path, Program prog,
                                              vector<smt::SortedVar> freeVars,
                                              bool toEnd) {
    const int progIndex = programIndex(prog);
    const auto filteredFreeVars = filterVars(progIndex, freeVars);

    vector<AssignmentCallBlock> allDefs;
    set<string> constructed;
    vector<CallInfo> callInfos;

    // Set the new values to the initial values
    vector<DefOrCallInfo> oldDefs;
    for (auto var : filteredFreeVars) {
        oldDefs.push_back(DefOrCallInfo(
            make_shared<Assignment>(var.name, stringExpr(var.name + "_old"))));
    }
    allDefs.push_back(AssignmentCallBlock(oldDefs, nullptr));

    // First block of path, this is special, because we don’t have a
    // previous
    // block so we can’t resolve phi nodes
    const auto startDefs = blockAssignments(*path.Start, nullptr, false, prog);
    allDefs.push_back(AssignmentCallBlock(startDefs, nullptr));

    auto prev = path.Start;

    // Rest of the path
    unsigned int i = 0;
    for (auto edge : path.Edges) {
        i++;
        const bool last = i == path.Edges.size();
        const auto defs =
            blockAssignments(*edge.Block, prev, last && !toEnd, prog);
        allDefs.push_back(AssignmentCallBlock(
            defs, edge.Cond == nullptr ? nullptr : edge.Cond->toSmt()));
        prev = edge.Block;
    }
    return allDefs;
}

SharedSMTRef addAssignments(const SharedSMTRef end,
                            vector<AssignmentBlock> assignments) {
    SharedSMTRef clause = end;
    for (auto assgns : makeReverse(assignments)) {
        clause = nestLets(clause, assgns.definitions);
        if (assgns.condition) {
            clause = makeOp("=>", assgns.condition, clause);
        }
    }
    return clause;
}

SharedSMTRef interleaveAssignments(
    SharedSMTRef endClause,
    MonoPair<vector<AssignmentCallBlock>> AssignmentCallBlocks,
    vector<SortedVar> &variableDeclarations) {
    SharedSMTRef clause = endClause;
    const auto splitAssignments =
        AssignmentCallBlocks.map<SplitAssignments>(splitAssignmentsFromCalls);
    const auto assignmentBlocks1 = splitAssignments.first.assignments;
    const auto assignmentBlocks2 = splitAssignments.second.assignments;
    const auto callInfo1 = splitAssignments.first.callInfos;
    const auto callInfo2 = splitAssignments.second.callInfos;

    const auto interleaveSteps = matchFunCalls(callInfo1, callInfo2);

    assert(assignmentBlocks1.size() == callInfo1.size() + 1);
    assert(assignmentBlocks2.size() == callInfo2.size() + 1);
    assert(AssignmentCallBlocks.first.size() >= 1);
    assert(AssignmentCallBlocks.second.size() >= 1);

    auto callIt1 = callInfo1.rbegin();
    auto callIt2 = callInfo2.rbegin();
    auto assignmentIt1 = assignmentBlocks1.rbegin();
    auto assignmentIt2 = assignmentBlocks2.rbegin();

    // We step through the matched calls in reverse to build up the smt from
    // the
    // bottom up
    for (InterleaveStep step : makeReverse(interleaveSteps)) {
        switch (step) {
        case InterleaveStep::StepFirst:
            clause = addAssignments(clause, *assignmentIt1);
            clause = nonmutualRecursiveForall(
                clause, *callIt1, variableDeclarations, Program::First);
            ++callIt1;
            ++assignmentIt1;
            break;
        case InterleaveStep::StepSecond:
            clause = addAssignments(clause, *assignmentIt2);
            clause = nonmutualRecursiveForall(
                clause, *callIt2, variableDeclarations, Program::Second);
            ++callIt2;
            ++assignmentIt2;
            break;
        case InterleaveStep::StepBoth:
            assert(coupledCalls(*callIt1, *callIt2));
            clause = addAssignments(clause, *assignmentIt2);
            clause = addAssignments(clause, *assignmentIt1);
            clause = mutualRecursiveForall(
                clause, makeMonoPair(*callIt1, *callIt2), variableDeclarations);
            ++callIt1;
            ++callIt2;
            ++assignmentIt1;
            ++assignmentIt2;
            break;
        }
    }
    // There is always one more block than there are calls, so we have to
    // add it
    // separately at the end
    clause = addAssignments(clause, *assignmentIt2);
    clause = addAssignments(clause, *assignmentIt1);
    ++assignmentIt1;
    ++assignmentIt2;

    assert(callIt1 == callInfo1.rend());
    assert(callIt2 == callInfo2.rend());
    assert(assignmentIt1 == assignmentBlocks1.rend());
    assert(assignmentIt2 == assignmentBlocks2.rend());

    return clause;
}

SharedSMTRef nonmutualSMT(SharedSMTRef endClause,
                          vector<AssignmentCallBlock> assignmentCallBlocks,
                          vector<SortedVar> &variableDeclarations,
                          Program prog) {
    SharedSMTRef clause = endClause;
    const auto splitAssignments =
        splitAssignmentsFromCalls(assignmentCallBlocks);
    const auto assignmentBlocks = splitAssignments.assignments;
    const auto callInfos = splitAssignments.callInfos;
    assert(assignmentBlocks.size() == callInfos.size() + 1);
    bool first = true;
    auto callIt = callInfos.rbegin();
    for (auto assgnsVec : makeReverse(assignmentBlocks)) {
        if (first) {
            first = false;
        } else {
            clause = nonmutualRecursiveForall(clause, *callIt,
                                              variableDeclarations, prog);
            ++callIt;
        }
        clause = addAssignments(clause, assgnsVec);
    }
    return clause;
}

/* -------------------------------------------------------------------------- */
// Functions to generate various foralls

SMTRef mutualRecursiveForall(SharedSMTRef clause, MonoPair<CallInfo> callPair,
                             vector<SortedVar> &variableDeclarations) {
    const uint32_t varArgs = callPair.first.varArgs;
    vector<SortedVar> args;
    args.push_back(
        SortedVar(callPair.first.assignedTo,
                  llvmTypeToSMTSort(callPair.first.fun.getReturnType())));
    args.push_back(
        SortedVar(callPair.second.assignedTo,
                  llvmTypeToSMTSort(callPair.second.fun.getReturnType())));
    if (SMTGenerationOpts::getInstance().Heap) {
        args.push_back(SortedVar("HEAP$1_res", arrayType()));
        args.push_back(SortedVar("HEAP$2_res", arrayType()));
    }
    vector<SharedSMTRef> implArgs;

    callPair.indexedForEach(addMemory(implArgs));
    vector<SharedSMTRef> preArgs = implArgs;

    implArgs.push_back(stringExpr(callPair.first.assignedTo));
    implArgs.push_back(stringExpr(callPair.second.assignedTo));
    if (SMTGenerationOpts::getInstance().Heap) {
        implArgs.push_back(stringExpr("HEAP$1_res"));
        implArgs.push_back(stringExpr("HEAP$2_res"));
    }
    SMTRef postInvariant = std::make_unique<Op>(
        invariantName(ENTRY_MARK, ProgramSelection::Both,
                      callPair.first.callName + "^" + callPair.second.callName,
                      InvariantAttr::NONE, varArgs),
        implArgs, !callPair.first.externFun);
    SMTRef result = makeOp("=>", std::move(postInvariant), clause);
    if (SMTGenerationOpts::getInstance().MuZ) {
        variableDeclarations.insert(variableDeclarations.end(), args.begin(),
                                    args.end());
    } else {
        result = std::make_unique<Forall>(args, std::move(result));
    }
    if (callPair.first.externFun) {
        return result;
    }
    SMTRef preInv = std::make_unique<Op>(
        invariantName(ENTRY_MARK, ProgramSelection::Both,
                      callPair.first.callName + "^" + callPair.second.callName,
                      InvariantAttr::PRE),
        preArgs);
    return makeOp("and", std::move(preInv), std::move(result));
}

SMTRef nonmutualRecursiveForall(SharedSMTRef clause, CallInfo call,
                                vector<SortedVar> &variableDeclarations,
                                Program prog) {
    vector<SortedVar> forallArgs;
    vector<SharedSMTRef> implArgs;

    const int progIndex = programIndex(prog);
    const string programS = std::to_string(progIndex);

    const uint32_t varArgs = call.varArgs;
    forallArgs.push_back(SortedVar(call.assignedTo, "Int"));
    if (SMTGenerationOpts::getInstance().Heap) {
        forallArgs.push_back(
            SortedVar("HEAP$" + programS + "_res", arrayType()));
    }
    addMemory(implArgs)(call, progIndex);
    const vector<SharedSMTRef> preArgs = implArgs;

    implArgs.push_back(stringExpr(call.assignedTo));
    if (SMTGenerationOpts::getInstance().Heap) {
        implArgs.push_back(stringExpr("HEAP$" + programS + "_res"));
    }

    const SharedSMTRef endInvariant = make_shared<Op>(
        invariantName(ENTRY_MARK, asSelection(prog), call.callName,
                      InvariantAttr::NONE, varArgs),
        implArgs, !call.externFun);
    SMTRef result = makeOp("=>", endInvariant, clause);
    if (SMTGenerationOpts::getInstance().MuZ) {
        variableDeclarations.insert(variableDeclarations.end(),
                                    forallArgs.begin(), forallArgs.end());
    } else {
        result = std::make_unique<Forall>(forallArgs, std::move(result));
    }
    if (call.externFun) {
        return result;
    }
    const auto preInv =
        make_shared<Op>(invariantName(ENTRY_MARK, asSelection(prog),
                                      call.callName, InvariantAttr::PRE),
                        preArgs);
    return makeOp("and", preInv, std::move(result));
}

/// Wrap the clause in a forall
SharedSMTRef forallStartingAt(SharedSMTRef clause, vector<SortedVar> freeVars,
                              int blockIndex, ProgramSelection prog,
                              string funName, bool main,
                              smt::FreeVarsMap freeVarsMap) {
    vector<SortedVar> vars;
    vector<string> preVars;
    for (const auto &arg : freeVars) {
        std::smatch matchResult;
        vars.push_back(toSMTSortedVar(SortedVar(arg.name + "_old", arg.type)));
        preVars.push_back(arg.name + "_old");
    }

    if (vars.empty()) {
        return clause;
    }

    if (main && blockIndex == ENTRY_MARK) {
        string opname =
            SMTGenerationOpts::getInstance().InitPredicate ? "INIT" : "IN_INV";

        vector<string> args;
        for (const auto &arg : freeVars) {
            args.push_back(arg.name + "_old");
        }

        clause = makeOp("=>", makeOp(opname, args), clause);

    } else {
        InvariantAttr attr = main ? InvariantAttr::MAIN : InvariantAttr::PRE;
        SMTRef preInv =
            makeOp(invariantName(blockIndex, prog, funName, attr), preVars);
        clause = makeOp("=>", std::move(preInv), clause);
    }

    if (SMTGenerationOpts::getInstance().MuZ ||
        SMTGenerationOpts::getInstance().Invert) {
        // μZ rules are implicitly universally quantified
        return clause;
    }
    return std::make_unique<Forall>(vars, clause);
}

/* -------------------------------------------------------------------------- */
// Functions forcing arguments to be equal

SharedSMTRef makeFunArgsEqual(SharedSMTRef clause, SharedSMTRef preClause,
                              vector<smt::SortedVar> Args1,
                              vector<smt::SortedVar> Args2) {
    assert(Args1.size() == Args2.size());

    vector<string> args;
    for (const auto &arg : Args1) {
        args.push_back(arg.name);
    }
    for (const auto &arg : Args2) {
        args.push_back(arg.name);
    }

    auto inInv = makeOp("IN_INV", args);

    return makeOp("=>", std::move(inInv), makeOp("and", clause, preClause));
}

/// Create an assertion to require that if the recursive invariant holds and the
/// arguments are equal the outputs are equal
SharedSMTRef equalInputsEqualOutputs(vector<smt::SortedVar> funArgs,
                                     vector<smt::SortedVar> funArgs1,
                                     vector<smt::SortedVar> funArgs2,
                                     string funName,
                                     smt::FreeVarsMap freeVarsMap,
                                     const llvm::Type *returnType) {
    vector<SortedVar> forallArgs;
    vector<string> args;
    vector<string> preInvArgs;
    for (const auto &arg : funArgs) {
        args.push_back(arg.name);
    }
    preInvArgs = args;

    forallArgs.insert(forallArgs.end(), funArgs.begin(), funArgs.end());

    args.push_back("result$1");
    args.push_back("result$2");
    forallArgs.push_back(SortedVar("result$1", llvmTypeToSMTSort(returnType)));
    forallArgs.push_back(SortedVar("result$2", llvmTypeToSMTSort(returnType)));
    if (SMTGenerationOpts::getInstance().Heap) {
        forallArgs.push_back(SortedVar("HEAP$1_res", arrayType()));
        forallArgs.push_back(SortedVar("HEAP$2_res", arrayType()));
        args.push_back("HEAP$1_res");
        args.push_back("HEAP$2_res");
    }
    vector<string> outArgs = {"result$1", "result$2"};
    vector<string> sortedFunArgs1;
    vector<string> sortedFunArgs2;
    for (const auto &arg : funArgs1) {
        sortedFunArgs1.push_back(arg.name);
    }
    for (const auto &arg : funArgs2) {
        sortedFunArgs2.push_back(arg.name);
    }
    std::sort(sortedFunArgs1.begin(), sortedFunArgs1.end());
    std::sort(sortedFunArgs2.begin(), sortedFunArgs2.end());
    if (SMTGenerationOpts::getInstance().PassInputThrough) {
        for (const auto &arg : funArgs1) {
            if (!std::regex_match(arg.name, HEAP_REGEX)) {
                outArgs.push_back(arg.name);
            }
        }
    }
    if (SMTGenerationOpts::getInstance().Heap) {
        outArgs.push_back("HEAP$1_res");
    }
    if (SMTGenerationOpts::getInstance().PassInputThrough) {
        for (const auto &arg : funArgs2) {
            if (!std::regex_match(arg.name, HEAP_REGEX)) {
                outArgs.push_back(arg.name);
            }
        }
    }
    if (SMTGenerationOpts::getInstance().Heap) {
        outArgs.push_back("HEAP$2_res");
    }
    const SharedSMTRef equalResults = makeOp(
        "=>", makeOp(invariantName(ENTRY_MARK, ProgramSelection::Both, funName),
                     args),
        makeOp("OUT_INV", outArgs));
    SMTRef preInv = makeOp(invariantName(ENTRY_MARK, ProgramSelection::Both,
                                         funName, InvariantAttr::PRE),
                           preInvArgs);

    const auto equalArgs =
        makeFunArgsEqual(equalResults, std::move(preInv), funArgs1, funArgs2);
    const auto forallInputs = make_shared<Forall>(forallArgs, equalArgs);
    return make_shared<Assert>(forallInputs);
}

/* -------------------------------------------------------------------------- */
// Functions  related to the search for free variables

/// Collect the free variables for the entry block of the PathMap
/// A variable is free if we use it but didn't create it
std::pair<set<FreeVar>, map<int, set<FreeVar>>>
freeVarsForBlock(map<int, Paths> pathMap) {
    set<FreeVar> freeVars;
    map<int, set<FreeVar>> constructedIntersection;
    for (const auto &paths : pathMap) {
        for (const auto &path : paths.second) {
            const llvm::BasicBlock *prev = path.Start;
            set<FreeVar> constructed;

            // the first block is special since we can't resolve phi
            // nodes here
            for (auto &instr : *path.Start) {
                constructed.insert(llvmValToFreeVar(&instr));
                if (llvm::isa<llvm::PHINode>(instr)) {
                    freeVars.insert(llvmValToFreeVar(&instr));
                    continue;
                }
                if (const auto callInst =
                        llvm::dyn_cast<llvm::CallInst>(&instr)) {
                    for (unsigned int i = 0; i < callInst->getNumArgOperands();
                         i++) {
                        const auto arg = callInst->getArgOperand(i);
                        if (!arg->getName().empty() &&
                            constructed.find(llvmValToFreeVar(arg)) ==
                                constructed.end()) {
                            freeVars.insert(llvmValToFreeVar(arg));
                        }
                    }
                    continue;
                }
                for (const auto op : instr.operand_values()) {
                    if (constructed.find(llvmValToFreeVar(op)) ==
                            constructed.end() &&
                        !op->getName().empty() &&
                        !llvm::isa<llvm::BasicBlock>(op)) {
                        freeVars.insert(llvmValToFreeVar(op));
                    }
                }
            }

            // now deal with the rest
            for (const auto &edge : path.Edges) {
                for (auto &instr : *edge.Block) {
                    constructed.insert(llvmValToFreeVar(&instr));
                    if (const auto phiInst =
                            llvm::dyn_cast<llvm::PHINode>(&instr)) {
                        const auto incoming =
                            phiInst->getIncomingValueForBlock(prev);
                        if (constructed.find(llvmValToFreeVar(incoming)) ==
                                constructed.end() &&
                            !incoming->getName().empty() &&
                            !llvm::isa<llvm::BasicBlock>(incoming)) {
                            freeVars.insert(llvmValToFreeVar(incoming));
                        }
                        continue;
                    }
                    if (const auto callInst =
                            llvm::dyn_cast<llvm::CallInst>(&instr)) {
                        for (unsigned int i = 0;
                             i < callInst->getNumArgOperands(); i++) {
                            const auto arg = callInst->getArgOperand(i);
                            if (!arg->getName().empty() &&
                                constructed.find(llvmValToFreeVar(arg)) ==
                                    constructed.end()) {
                                freeVars.insert(llvmValToFreeVar(arg));
                            }
                        }
                        continue;
                    }
                    for (const auto op : instr.operand_values()) {
                        if (constructed.find(llvmValToFreeVar(op)) ==
                                constructed.end() &&
                            !op->getName().empty() &&
                            !llvm::isa<llvm::BasicBlock>(op)) {
                            freeVars.insert(llvmValToFreeVar(op));
                        }
                    }
                }
                prev = edge.Block;
            }

            set<FreeVar> newConstructedIntersection;
            if (constructedIntersection.find(paths.first) ==
                constructedIntersection.end()) {
                constructedIntersection.insert(
                    make_pair(paths.first, constructed));
                ;
            } else {
                std::set_intersection(
                    constructed.begin(), constructed.end(),
                    constructedIntersection.at(paths.first).begin(),
                    constructedIntersection.at(paths.first).end(),
                    inserter(newConstructedIntersection,
                             newConstructedIntersection.begin()));
                constructedIntersection.insert(
                    make_pair(paths.first, newConstructedIntersection));
            }
        }
    }
    return make_pair(freeVars, constructedIntersection);
}

smt::FreeVarsMap freeVars(PathMap map1, PathMap map2,
                          vector<smt::SortedVar> funArgs) {
    map<int, set<SortedVar>> freeVarsMap;
    smt::FreeVarsMap freeVarsMapVect;
    map<int, map<int, set<SortedVar>>> constructed;
    for (const auto &it : map1) {
        const int index = it.first;
        auto freeVars1 = freeVarsForBlock(map1.at(index));
        const auto freeVars2 = freeVarsForBlock(map2.at(index));
        for (auto var : freeVars2.first) {
            freeVars1.first.insert(var);
        }
        set<SortedVar> freeVars;
        for (const auto var : freeVars1.first) {
            freeVars.insert(var.var);
            if (SMTGenerationOpts::getInstance().Stack &&
                var.type->isPointerTy()) {
                freeVars.insert({var.var.name + "_OnStack", "Bool"});
            }
        }
        freeVarsMap.insert(make_pair(index, freeVars));

        // constructed variables
        for (auto mapIt : freeVars2.second) {
            for (auto var : mapIt.second) {
                freeVars1.second[mapIt.first].insert(var);
            }
        }

        map<int, set<SortedVar>> constructedVarsMap;
        for (const auto &it : freeVars1.second) {
            set<SortedVar> constructedVars;
            for (const auto var : it.second) {
                constructedVars.insert(var.var);
                if (var.type->isPointerTy()) {
                    constructedVars.insert({var.var.name + "_OnStack", "Bool"});
                }
            }
            constructedVarsMap.insert({it.first, constructedVars});
        }

        constructed.insert(make_pair(index, constructedVarsMap));
    }

    freeVarsMap[EXIT_MARK] = set<smt::SortedVar>();
    if (SMTGenerationOpts::getInstance().PassInputThrough) {
        for (const auto &arg : funArgs) {
            freeVarsMap[EXIT_MARK].insert(arg);
        }
    }
    freeVarsMap[UNREACHABLE_MARK] = set<smt::SortedVar>();

    // search for a least fixpoint
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &it : map1) {
            const int startIndex = it.first;
            for (const auto &itInner : it.second) {
                const int endIndex = itInner.first;
                for (auto var : freeVarsMap.at(endIndex)) {
                    if (constructed.at(startIndex).at(endIndex).find(var) ==
                        constructed.at(startIndex).at(endIndex).end()) {
                        const auto inserted =
                            freeVarsMap.at(startIndex).insert(var);
                        changed = changed || inserted.second;
                    }
                }
            }
        }
    }

    const auto addMemoryArrays = [](vector<smt::SortedVar> vars,
                                    int index) -> vector<smt::SortedVar> {
        if (SMTGenerationOpts::getInstance().Heap) {
            vars.push_back(SortedVar(heapName(index), arrayType()));
        }
        if (SMTGenerationOpts::getInstance().Stack) {
            vars.push_back(
                SortedVar(stackPointerName(index), stackPointerType()));
            vars.push_back(SortedVar(stackName(index), arrayType()));
        }
        return vars;
    };

    for (auto it : freeVarsMap) {
        const int index = it.first;
        vector<smt::SortedVar> varsVect;
        for (const auto &var : it.second) {
            varsVect.push_back(var);
        }
        const auto argPair =
            makeMonoPair(filterVars(1, varsVect), filterVars(2, varsVect))
                .indexedMap<vector<smt::SortedVar>>(addMemoryArrays);
        freeVarsMapVect[index] = concat(argPair);
    }

    const auto argPair =
        makeMonoPair(filterVars(1, funArgs), filterVars(2, funArgs))
            .indexedMap<vector<smt::SortedVar>>(addMemoryArrays);
    // The input arguments should be in the function call order so we can’t add
    // them before
    freeVarsMapVect[ENTRY_MARK] = concat(argPair);

    return freeVarsMapVect;
}

/* -------------------------------------------------------------------------- */
// Miscellanous helper functions that don't really belong anywhere

MonoPair<vector<smt::SortedVar>> functionArgs(const llvm::Function &fun1,
                                              const llvm::Function &fun2) {
    vector<smt::SortedVar> args1;
    for (auto &arg : fun1.args()) {
        auto sVar = llvmValToSortedVar(&arg);
        args1.push_back(sVar);
        if (SMTGenerationOpts::getInstance().Stack &&
            arg.getType()->isPointerTy()) {
            args1.push_back({sVar.name + "_OnStack", "Bool"});
        }
    }
    vector<smt::SortedVar> args2;
    for (auto &arg : fun2.args()) {
        auto sVar = llvmValToSortedVar(&arg);
        args2.push_back(sVar);
        if (SMTGenerationOpts::getInstance().Stack &&
            arg.getType()->isPointerTy()) {
            args2.push_back({sVar.name + "_OnStack", "Bool"});
        }
    }
    return makeMonoPair(args1, args2);
}

auto functionArgsFreeVars(const llvm::Function &fun1,
                          const llvm::Function &fun2)
    -> std::vector<smt::SortedVar> {
    return concat(functionArgs(fun1, fun2));
}

/// Swap the program index
int swapIndex(int I) {
    assert(I == 1 || I == 2);
    return I == 1 ? 2 : 1;
}

/// Split the assignment blocks on each call
SplitAssignments
splitAssignmentsFromCalls(vector<AssignmentCallBlock> assignmentCallBlocks) {
    vector<vector<AssignmentBlock>> assignmentBlocks;
    vector<CallInfo> callInfos;
    vector<struct AssignmentBlock> currentAssignmentsList;
    for (auto assignments : assignmentCallBlocks) {
        SharedSMTRef condition = assignments.condition;
        vector<Assignment> currentDefinitions;
        for (auto defOrCall : assignments.definitions) {
            if (defOrCall.tag == DefOrCallInfoTag::Def) {
                currentDefinitions.push_back(*defOrCall.definition);
            } else {
                currentAssignmentsList.push_back(
                    AssignmentBlock(currentDefinitions, condition));
                currentDefinitions.clear();
                assignmentBlocks.push_back(currentAssignmentsList);
                currentAssignmentsList.clear();
                condition = nullptr;
                callInfos.push_back(*defOrCall.callInfo);
            }
        }
        currentAssignmentsList.push_back(
            AssignmentBlock(currentDefinitions, condition));
    }
    assignmentBlocks.push_back(currentAssignmentsList);
    return {assignmentBlocks, callInfos};
}

vector<InterleaveStep> matchFunCalls(vector<CallInfo> callInfos1,
                                     vector<CallInfo> callInfos2) {
    // This is just a basic edit distance algorithm
    vector<vector<size_t>> table(callInfos1.size() + 1,
                                 vector<size_t>(callInfos2.size() + 1, 0));
    for (uint32_t i = 0; i <= callInfos1.size(); ++i) {
        table[i][0] = i;
    }
    for (uint32_t j = 0; j <= callInfos2.size(); ++j) {
        table[0][j] = j;
    }
    for (uint32_t i = 1; i <= callInfos1.size(); ++i) {
        for (uint32_t j = 1; j <= callInfos2.size(); ++j) {
            if (coupledCalls(callInfos1[i - 1], callInfos2[j - 1])) {
                table[i][j] = table[i - 1][j - 1];
            } else {
                table[i][j] =
                    std::min(table[i - 1][j] + 1, table[i][j - 1] + 1);
            }
        }
    }
    vector<InterleaveStep> interleaveSteps;
    uint64_t i = callInfos1.size(), j = callInfos2.size();
    while (i > 0 && j > 0) {
        if (coupledCalls(callInfos1[i - 1], callInfos2[j - 1])) {
            interleaveSteps.push_back(InterleaveStep::StepBoth);
            --i;
            --j;
        } else {
            if (table[i - 1][j] <= table[i][j - 1]) {
                interleaveSteps.push_back(InterleaveStep::StepFirst);
                --i;
            } else {
                interleaveSteps.push_back(InterleaveStep::StepSecond);
                --j;
            }
        }
    }
    while (i > 0) {
        interleaveSteps.push_back(InterleaveStep::StepFirst);
        --i;
    }
    while (j > 0) {
        interleaveSteps.push_back(InterleaveStep::StepSecond);
        --j;
    }
    std::reverse(interleaveSteps.begin(), interleaveSteps.end());
    return interleaveSteps;
}

/// Check if the marks match
void checkPathMaps(PathMap map1, PathMap map2) {
    if (!mapSubset(map1, map2) || !mapSubset(map2, map1)) {
        exit(1);
    }
}

bool mapSubset(PathMap map1, PathMap map2) {
    for (auto Pair : map1) {
        if (map2.find(Pair.first) == map2.end()) {
            logError("Mark '" + std::to_string(Pair.first) +
                     "' doesn’t exist in both files\n");
            return false;
        }
    }
    return true;
}

SMTRef getDontLoopInvariant(SMTRef endClause, int startIndex, PathMap pathMap,
                            smt::FreeVarsMap freeVars,
                            vector<SortedVar> &variableDeclarations,
                            Program prog) {
    SMTRef clause = std::move(endClause);
    vector<Path> dontLoopPaths;
    for (auto pathMapIt : pathMap.at(startIndex)) {
        if (pathMapIt.first == startIndex) {
            for (auto path : pathMapIt.second) {
                dontLoopPaths.push_back(path);
            }
        }
    }
    vector<SharedSMTRef> dontLoopExprs;
    for (auto path : dontLoopPaths) {
        auto defs =
            assignmentsOnPath(path, prog, freeVars.at(startIndex), false);
        auto smt =
            nonmutualSMT(stringExpr("false"), defs, variableDeclarations, prog);
        dontLoopExprs.push_back(smt);
    }
    if (!dontLoopExprs.empty()) {
        auto andExpr = make_shared<Op>("and", dontLoopExprs);
        clause = makeOp("=>", andExpr, std::move(clause));
    }
    return clause;
}

auto addMemory(vector<SharedSMTRef> &implArgs)
    -> function<void(CallInfo call, int index)> {
    return [&implArgs](CallInfo call, int index) {
        for (auto arg : call.args) {
            implArgs.push_back(arg);
        }
        if (SMTGenerationOpts::getInstance().Heap) {
            implArgs.push_back(stringExpr(heapName(index)));
        }
        if (SMTGenerationOpts::getInstance().Stack) {
            implArgs.push_back(stringExpr(stackPointerName(index)));
            implArgs.push_back(stringExpr(stackName(index)));
        }
    };
}

void declareVariables(smt::FreeVarsMap freeVarsMap,
                      std::vector<SortedVar> &variableDeclarations) {
    set<SortedVar> uniqueVars;
    for (auto it : freeVarsMap) {
        for (const auto &var : it.second) {
            uniqueVars.insert(var);
        }
    }
    vector<SharedSMTRef> variables;
    for (const auto &var : uniqueVars) {
        variableDeclarations.push_back({var.name + "_old", var.type});
    }
    if (SMTGenerationOpts::getInstance().Invert) {
        variableDeclarations.push_back({"INV_INDEX", "Int"});
    }
}

auto dropTypesFreeVars(smt::FreeVarsMap map)
    -> std::map<int, std::vector<std::string>> {
    std::map<int, vector<string>> result;
    for (auto it : map) {
        vector<string> vars;
        for (auto &sortedVar : it.second) {
            vars.push_back(sortedVar.name);
        }
        result.insert(std::make_pair(it.first, vars));
    }
    return result;
}

FreeVar llvmValToFreeVar(const llvm::Value *val) {
    return {llvmValToSortedVar(val), val->getType()};
}

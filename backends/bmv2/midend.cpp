#include "midend.h"
#include "lower.h"
#include "inlining.h"
#include "midend/actionsInlining.h"
#include "midend/uniqueNames.h"
#include "midend/moveDeclarations.h"
#include "midend/removeReturns.h"
#include "midend/moveConstructors.h"
#include "frontends/p4/strengthReduction.h"
#include "frontends/common/typeMap.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/typeChecking/typeChecker.h"
#include "frontends/common/resolveReferences/resolveReferences.h"
#include "frontends/p4/toP4/toP4.h"
#include "frontends/p4/simplify.h"
#include "frontends/p4/unusedDeclarations.h"
#include "frontends/common/constantFolding.h"

namespace BMV2 {

const IR::P4Program* MidEnd::processV1(CompilerOptions&, const IR::P4Program* program) {
    bool isv1 = true;
    P4::ReferenceMap refMap;
    P4::TypeMap typeMap;

    auto evaluator0 = new P4::EvaluatorPass(true);
    (void)program->apply(*evaluator0);
    if (::errorCount() > 0)
        return nullptr;
    
    // Inlining is simpler for P4 v1.0/1.1 programs, so we have a
    // specialized code path, which also generates slighly nicer
    // human-readable results.  
    P4::InlineWorkList controlsToInline;
    P4::ActionsInlineList actionsToInline;
    
    PassManager midend = {
        new P4::DiscoverInlining(&controlsToInline, evaluator0->getBlockMap()),
        new P4::InlineDriver(&controlsToInline, new SimpleControlsInliner(&refMap), isv1),
        new PassRepeated {
            // remove useless callees
            new P4::ResolveReferences(&refMap, isv1),
            new P4::RemoveUnusedDeclarations(&refMap),
        },
        new P4::ResolveReferences(&refMap, isv1),
        new P4::TypeChecker(&refMap, &typeMap, true, true),
        new P4::DiscoverActionsInlining(&actionsToInline, &refMap, &typeMap),
        new P4::InlineActionsDriver(&actionsToInline, new SimpleActionsInliner(&refMap), isv1),
        new PassRepeated {
            new P4::ResolveReferences(&refMap, isv1),
            new P4::RemoveUnusedDeclarations(&refMap),
        }
    };
    midend.setName("Mid end");
    midend.setStopOnError(true);
    program = program->apply(midend);
    if (::errorCount() > 0)
        return nullptr;
    return program;
}

const IR::P4Program* MidEnd::processV1_2(CompilerOptions&, const IR::P4Program* program) {
    bool isv1 = false;
    auto evaluator0 = new P4::EvaluatorPass(isv1);
    P4::ReferenceMap refMap;

    PassManager simplify = {
        // Give each local declaration a unique internal name
        new P4::UniqueNames(isv1),
        // Move all local declarations to the beginning
        new P4::MoveDeclarations(),
        new P4::ResolveReferences(&refMap, isv1),
        new P4::RemoveReturns(&refMap, true),
        // Move some constructor calls into temporaries
        new P4::MoveConstructors(isv1),
        new P4::ResolveReferences(&refMap, isv1),
        new P4::RemoveUnusedDeclarations(&refMap),
        evaluator0,
    };

    simplify.setName("Simplify");
    simplify.setStopOnError(true);
    program = program->apply(simplify);
    if (::errorCount() > 0)
        return nullptr;
    auto blockMap = evaluator0->getBlockMap();
    if (blockMap->getMain() == nullptr)
        // nothing further to do
        return nullptr;

    P4::TypeMap typeMap;
    P4::InlineWorkList toInline;
    P4::ActionsInlineList actionsToInline;
    PassManager midEnd = {
        new P4::DiscoverInlining(&toInline, blockMap),
        new P4::InlineDriver(&toInline, new P4::GeneralInliner(), isv1),
        new PassRepeated {
            // remove useless callees
            new P4::ResolveReferences(&refMap, isv1),
            new P4::RemoveUnusedDeclarations(&refMap),
        },
        new P4::ResolveReferences(&refMap, isv1),
        new P4::TypeChecker(&refMap, &typeMap, true, true),
        new P4::DiscoverActionsInlining(&actionsToInline, &refMap, &typeMap),
        new P4::InlineActionsDriver(&actionsToInline, new P4::ActionsInliner(), isv1),
        new PassRepeated {
            new P4::ResolveReferences(&refMap, isv1),
            new P4::RemoveUnusedDeclarations(&refMap),
        },
        new P4::SimplifyControlFlow(),
        new P4::ResolveReferences(&refMap, isv1),
        new P4::TypeChecker(&refMap, &typeMap),
        new P4::ConstantFolding(&refMap, &typeMap),
        new P4::StrengthReduction(),
        new P4::UniqueNames(isv1),
        new P4::MoveDeclarations(),

        // TODO: this is not sufficient.
        // For example, we do not create new actions for statements in
        // the control-flow.
    };

    midEnd.setName("Mid end");
    midEnd.setStopOnError(true);
    program = program->apply(midEnd);
    if (::errorCount() > 0)
        return nullptr;
    return program;
}


P4::BlockMap* MidEnd::process(CompilerOptions& options, const IR::P4Program* program) {
    bool isv1 = options.langVersion == CompilerOptions::FrontendVersion::P4v1;
    if (isv1) 
        program = processV1(options, program);
    else 
        program = processV1_2(options, program);
    if (program == nullptr)
        return nullptr;
    
    std::ostream *midendStream = options.dumpStream("-midend");
    // BMv2-specific passes
    P4::ReferenceMap refMap;
    P4::TypeMap typeMap;
    auto evaluator1 = new P4::EvaluatorPass(isv1);
    PassManager backend = {
        new P4::ToP4(midendStream, options.file),
        new P4::SimplifyControlFlow(),
        new P4::ResolveReferences(&refMap, isv1),
        new P4::TypeChecker(&refMap, &typeMap, true, true),
        new RemoveLeftSlices(&typeMap),
        new P4::ResolveReferences(&refMap, isv1),
        new P4::TypeChecker(&refMap, &typeMap, true, true),
        new LowerExpressions(&typeMap),
        new P4::ResolveReferences(&refMap, isv1),
        new P4::TypeChecker(&refMap, &typeMap, true, true),
        new P4::ConstantFolding(&refMap, &typeMap),
        evaluator1
    };
    
    backend.setName("Backend");
    backend.setStopOnError(true);
    program = program->apply(backend);
    if (::errorCount() > 0)
        return nullptr;
    
    auto blockMap = evaluator1->getBlockMap();
    return blockMap;
}

}  // namespace BMV2

/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003-2023 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "Parser.h"

#include "ASTBuilder.h"
#include "BuiltinNames.h"
#include "DebuggerParseData.h"
#include "JSCJSValueInlines.h"
#include "VM.h"
#include <utility>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StringPrintStream.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

#define updateErrorMessage(shouldPrintToken, ...) do {\
    propagateError(); \
    logError(shouldPrintToken, __VA_ARGS__); \
} while (0)

#define propagateError() do { if (hasError()) [[unlikely]] return 0; } while (0)
#define internalFailWithMessage(shouldPrintToken, ...) do { updateErrorMessage(shouldPrintToken, __VA_ARGS__); return 0; } while (0)
#define handleErrorToken() do { if (m_token.m_type == EOFTOK || m_token.m_type & CanBeErrorTokenFlag) { failDueToUnexpectedToken(); } } while (0)
#define failWithMessage(...) do { { handleErrorToken(); updateErrorMessage(true, __VA_ARGS__); } return 0; } while (0)
#define failWithStackOverflow() do { updateErrorMessage(false, "Stack exhausted"); m_hasStackOverflow = true; return 0; } while (0)
#define failIfFalse(cond, ...) do { if (!(cond)) [[unlikely]] { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define failIfTrue(cond, ...) do { if ((cond)) [[unlikely]] { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define failIfTrueIfStrict(cond, ...) do { if ((cond) && strictMode()) [[unlikely]] internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define failIfFalseIfStrict(cond, ...) do { if ((!(cond)) && strictMode()) [[unlikely]] internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define consumeOrFail(tokenType, ...) do { if (!consume(tokenType)) [[unlikely]] { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define consumeOrFailWithFlags(tokenType, flags, ...) do { if (!consume(tokenType, flags)) [[unlikely]] { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define matchOrFail(tokenType, ...) do { if (!match(tokenType)) [[unlikely]] { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define failIfStackOverflow() do { if (!canRecurse()) [[unlikely]] failWithStackOverflow(); } while (0)
#define semanticFail(...) do { internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define semanticFailIfTrue(cond, ...) do { if ((cond)) [[unlikely]] internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define semanticFailIfFalse(cond, ...) do { if (!(cond)) [[unlikely]] internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define regexFail(failure) do { setErrorMessage(failure); return 0; } while (0)
#define failDueToUnexpectedToken() do {\
        logError(true);\
    return 0;\
} while (0)

#define handleProductionOrFail(token, tokenString, operation, production) do {\
    consumeOrFail(token, "Expected '", tokenString, "' to ", operation, " a ", production);\
} while (0)

#define handleProductionOrFail2(token, tokenString, operation, production) do {\
    consumeOrFail(token, "Expected '", tokenString, "' to ", operation, " an ", production);\
} while (0)

#define semanticFailureDueToKeywordCheckingToken(token, ...) do { \
    semanticFailIfTrue(strictMode() && token.m_type == RESERVED_IF_STRICT, "Cannot use the reserved word '", getToken(token), "' as a ", __VA_ARGS__, " in strict mode"); \
    semanticFailIfTrue(token.m_type == RESERVED || token.m_type == RESERVED_IF_STRICT, "Cannot use the reserved word '", getToken(token), "' as a ", __VA_ARGS__); \
    if (token.m_type & KeywordTokenFlag) { \
        semanticFailIfFalse(isContextualKeyword(token), "Cannot use the keyword '", getToken(token), "' as a ", __VA_ARGS__); \
        semanticFailIfTrue(token.m_type == LET && strictMode(), "Cannot use 'let' as a ", __VA_ARGS__, " ", disallowedIdentifierLetReason()); \
        semanticFailIfTrue(token.m_type == AWAIT && !canUseIdentifierAwait(), "Cannot use 'await' as a ", __VA_ARGS__, " ", disallowedIdentifierAwaitReason()); \
        semanticFailIfTrue(token.m_type == YIELD && !canUseIdentifierYield(), "Cannot use 'yield' as a ", __VA_ARGS__, " ", disallowedIdentifierYieldReason()); \
    } \
} while (0)

#define semanticFailureDueToKeyword(...) semanticFailureDueToKeywordCheckingToken(m_token, __VA_ARGS__);

namespace JSC {

std::atomic<unsigned> globalParseCount { 0 };

WTF_MAKE_TZONE_ALLOCATED_IMPL(ModuleScopeData);

ALWAYS_INLINE static SourceParseMode getAsyncFunctionBodyParseMode(SourceParseMode parseMode)
{
    if (isAsyncGeneratorWrapperParseMode(parseMode))
        return SourceParseMode::AsyncGeneratorBodyMode;

    if (parseMode == SourceParseMode::AsyncArrowFunctionMode)
        return SourceParseMode::AsyncArrowFunctionBodyMode;

    return SourceParseMode::AsyncFunctionBodyMode;
}

template <typename LexerType>
void Parser<LexerType>::logError(bool)
{
    if (hasError())
        return;
    StringPrintStream stream;
    printUnexpectedTokenText(stream);
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType> template <typename... Args>
void Parser<LexerType>::logError(bool shouldPrintToken, Args&&... args)
{
    if (hasError())
        return;
    StringPrintStream stream;
    if (shouldPrintToken) {
        printUnexpectedTokenText(stream);
        stream.print(". ");
    }
    stream.print(std::forward<Args>(args)..., ".");
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType>
Parser<LexerType>::Parser(VM& vm, const SourceCode& source, ImplementationVisibility implementationVisibility, JSParserBuiltinMode builtinMode, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, SourceParseMode parseMode, FunctionMode functionMode, SuperBinding superBinding, ConstructorKind constructorKind, DerivedContextType derivedContextType, bool isEvalContext, EvalContextType evalContextType, DebuggerParseData* debuggerParseData, bool isInsideOrdinaryFunction)
    : m_vm(vm)
    , m_source(&source)
    , m_hasStackOverflow(false)
    , m_allowsIn(true)
    , m_statementDepth(0)
    , m_implementationVisibility(implementationVisibility)
    , m_parsingBuiltin(builtinMode == JSParserBuiltinMode::Builtin)
    , m_parseMode(parseMode)
    , m_functionMode(functionMode)
    , m_scriptMode(scriptMode)
    , m_superBinding(superBinding)
    , m_immediateParentAllowsFunctionDeclarationInStatement(false)
    , m_debuggerParseData(debuggerParseData)
    , m_isInsideOrdinaryFunction(isInsideOrdinaryFunction)
{
    m_lexer = makeUnique<LexerType>(vm, builtinMode, scriptMode);
    m_lexer->setCode(source, &m_parserArena);
    m_token.m_location.line = source.firstLine().oneBasedInt();
    m_token.m_location.startOffset = source.startOffset();
    m_token.m_location.endOffset = source.startOffset();
    m_token.m_location.lineStartOffset = source.startOffset();
    m_functionCache = vm.addSourceProviderCache(source.provider());
    m_expressionErrorClassifier = nullptr;

    ScopeRef scope = pushScope();
    scope->setLexicallyScopedFeatures(lexicallyScopedFeatures);
    scope->setSourceParseMode(parseMode);
    scope->setIsEvalContext(isEvalContext);
    if (isEvalContext)
        scope->setEvalContextType(evalContextType);

    if (scope->isFunction())
        scope->setConstructorKind(constructorKind);
    else
        ASSERT(constructorKind == ConstructorKind::None);

    scope->setDerivedContextType(derivedContextType);
    if (derivedContextType != DerivedContextType::None)
        scope->setExpectedSuperBinding(SuperBinding::Needed);

    if (isModuleParseMode(parseMode))
        m_moduleScopeData = ModuleScopeData::create();

    next();
}

class Scope::MaybeParseAsGeneratorFunctionForScope {
public:
    MaybeParseAsGeneratorFunctionForScope(ScopeRef& scope, bool shouldParseAsGeneratorFunction)
        : m_scope(scope)
        , m_oldValue(scope->m_isGeneratorFunction)
    {
        m_scope->m_isGeneratorFunction = shouldParseAsGeneratorFunction;
    }

    ~MaybeParseAsGeneratorFunctionForScope()
    {
        m_scope->m_isGeneratorFunction = m_oldValue;
    }

private:
    ScopeRef m_scope;
    bool m_oldValue;
};

struct DepthManager : private SetForScope<int> {
public:
    DepthManager(int* depth)
        : SetForScope(*depth, *depth)
    {
    }
};

template <typename LexerType>
Parser<LexerType>::~Parser()
{
}

void JSToken::dump(PrintStream& out) const
{
    out.print(*m_data.cooked);
}

static ALWAYS_INLINE bool isPrivateFieldName(UniquedStringImpl* uid)
{
    return uid->length() && uid->at(0) == '#';
}

template <typename LexerType>
Expected<typename Parser<LexerType>::ParseInnerResult, String> Parser<LexerType>::parseInner(const Identifier& calleeName, ParsingContext parsingContext, std::optional<int> functionConstructorParametersEndPosition, const FixedVector<UnlinkedFunctionExecutable::ClassElementDefinition>* classElementDefinitions, const PrivateNameEnvironment* parentScopePrivateNames)
{
    ASTBuilder context(const_cast<VM&>(m_vm), m_parserArena, const_cast<SourceCode*>(m_source));
    SourceParseMode parseMode = sourceParseMode();
    ScopeRef scope = currentScope();
    scope->setIsLexicalScope();

    bool hasPrivateNames = scope->isEvalContext() && parentScopePrivateNames && parentScopePrivateNames->size();

    if (hasPrivateNames) {
        scope->setIsPrivateNameScope();
        scope->lexicalVariables().addPrivateNamesFrom(parentScopePrivateNames);
    }

    SetForScope functionParsePhasePoisoner(m_parserState.functionParsePhase, FunctionParsePhase::Body);

    FunctionParameters* parameters = nullptr;
    bool isArrowFunctionBodyExpression = parseMode == SourceParseMode::AsyncArrowFunctionBodyMode && !match(OPENBRACE);
    if (m_lexer->isReparsingFunction()) {
        ParserFunctionInfo<ASTBuilder> functionInfo;
        if (isGeneratorOrAsyncFunctionBodyParseMode(parseMode))
            parameters = createGeneratorParameters(context, functionInfo.parameterCount);
        else if (parseMode == SourceParseMode::ClassFieldInitializerMode)
            parameters = context.createFormalParameterList();
        else
            parameters = parseFunctionParameters(context, functionInfo);

        if (SourceParseModeSet(SourceParseMode::ArrowFunctionMode, SourceParseMode::AsyncArrowFunctionMode).contains(parseMode) && !hasError()) {
            // FIXME:
            // Logically, this should be an assert, since we already successfully parsed the arrow
            // function when syntax checking. So logically, we should see the arrow token here.
            // But we're seeing crashes in the wild when making this an assert. Instead, we'll just
            // handle it as an error in release builds, and an assert on debug builds, with the hopes
            // of fixing it in the future.
            // https://bugs.webkit.org/show_bug.cgi?id=221633
            if (!match(ARROWFUNCTION)) [[unlikely]] {
                ASSERT_NOT_REACHED();
                return makeUnexpected("Parser error"_s);
            }
            next();
            isArrowFunctionBodyExpression = !match(OPENBRACE);
        }
    }

    if (functionNameIsInScope(calleeName, functionMode()))
        scope->declareCallee(&calleeName);

    if (m_lexer->isReparsingFunction())
        m_statementDepth--;

    SourceElements* sourceElements = nullptr;
    // The only way we can error this early is if we reparse a function and we run out of stack space.
    if (!hasError()) {
        if (isAsyncFunctionWrapperParseMode(parseMode))
            sourceElements = parseAsyncFunctionSourceElements(context, calleeName, isArrowFunctionBodyExpression, CheckForStrictMode);
        else if (isArrowFunctionBodyExpression)
            sourceElements = parseArrowFunctionSingleExpressionBodySourceElements(context);
        else if (isModuleParseMode(parseMode))
            sourceElements = parseModuleSourceElements(context);
        else if (isGeneratorWrapperParseMode(parseMode))
            sourceElements = parseGeneratorFunctionSourceElements(context, calleeName, CheckForStrictMode);
        else if (isAsyncGeneratorWrapperParseMode(parseMode))
            sourceElements = parseAsyncGeneratorFunctionSourceElements(context, calleeName, isArrowFunctionBodyExpression, CheckForStrictMode);
        else if (parsingContext == ParsingContext::FunctionConstructor)
            sourceElements = parseSingleFunction(context, functionConstructorParametersEndPosition);
        else if (parseMode == SourceParseMode::ClassFieldInitializerMode) {
            ASSERT(classElementDefinitions && !classElementDefinitions->isEmpty());
            sourceElements = parseClassFieldInitializerSourceElements(context, *classElementDefinitions);
        } else
            sourceElements = parseSourceElements(context, CheckForStrictMode);
    }

    bool validEnding = consume(EOFTOK);
    if (!sourceElements || !validEnding)
        return makeUnexpected(hasError() ? m_errorMessage : "Parser error"_s);

    if (!m_lexer->isReparsingFunction() && m_seenPrivateNameUseInNonReparsingFunctionMode) {
        String errorMessage;
        scope->forEachUsedVariable([&] (UniquedStringImpl* impl) {
            if (!isPrivateFieldName(impl))
                return IterationStatus::Continue;
            if (parentScopePrivateNames && parentScopePrivateNames->contains(impl))
                return IterationStatus::Continue;
            if (scope->lexicalVariables().contains(impl))
                return IterationStatus::Continue;
            errorMessage = makeString("Cannot reference undeclared private names: \""_s, StringView(impl), '"');
            return IterationStatus::Done;
        });
        if (!errorMessage.isNull())
            return makeUnexpected(errorMessage);
    }

    // It's essential to finalize the hoisting before computing captured variables.
    scope->finalizeSloppyModeFunctionHoisting();

    IdentifierSet capturedVariables;
    scope->getCapturedVars(capturedVariables);

    VariableEnvironment& varDeclarations = scope->declaredVariables();
    for (auto& entry : capturedVariables)
        varDeclarations.markVariableAsCaptured(entry.get());
    scope->finalizeLexicalEnvironment();

    if (isGeneratorWrapperParseMode(parseMode) || isAsyncFunctionOrAsyncGeneratorWrapperParseMode(parseMode)) {
        if (scope->usedVariablesContains(m_vm.propertyNames->arguments.impl()))
            context.propagateArgumentsUse();
    }

    CodeFeatures features = context.features();
    if (scope->shadowsArguments())
        features |= ShadowsArgumentsFeature;
    if (m_seenTaggedTemplateInNonReparsingFunctionMode)
        features |= NoEvalCacheFeature;
    if (scope->hasNonSimpleParameterList())
        features |= NonSimpleParameterListFeature;
    if (scope->usesImportMeta())
        features |= ImportMetaFeature;
    if (m_seenArgumentsDotLength && scope->hasDeclaredGlobalArguments())
        features |= ArgumentsFeature;

#if ASSERT_ENABLED
    if (m_parsingBuiltin && isProgramParseMode(parseMode)) {
        VariableEnvironment& lexicalVariables = scope->lexicalVariables();
        auto& closedVariableCandidates = scope->closedVariableCandidates();
        for (UniquedStringImpl* candidate : closedVariableCandidates) {
            // FIXME: We allow async to leak because it appearing as a closed variable is a side effect of trying to parse async arrow functions.
            if (!lexicalVariables.contains(candidate) && !varDeclarations.contains(candidate) && !candidate->isSymbol() && candidate != m_vm.propertyNames->async.impl()) {
                dataLog("Bad global capture in builtin: '", candidate, "'\n");
                dataLog(m_source->view());
                CRASH();
            }
        }
    }
#endif // ASSERT_ENABLED

    return ParseInnerResult { parameters, sourceElements, scope->takeFunctionDeclarations(), scope->takeDeclaredVariables(), scope->takeLexicalEnvironment(), features, context.numConstants() };
}

template <typename LexerType>
template <class TreeBuilder> bool Parser<LexerType>::isArrowFunctionParameters(TreeBuilder& context)
{
    if (match(OPENPAREN)) {
        SavePoint saveArrowFunctionPoint = createSavePoint(context);
        next();
        bool isArrowFunction = false;
        if (consume(CLOSEPAREN))
            isArrowFunction = match(ARROWFUNCTION);
        else {
            SyntaxChecker syntaxChecker(const_cast<VM&>(m_vm), m_lexer.get());
            // We make fake scope, otherwise parseFormalParameters will add variable to current scope that lead to errors
            AutoPopScopeRef fakeScope(this, pushScope());

            fakeScope->setSourceParseMode(SourceParseMode::ArrowFunctionMode);
            resetImplementationVisibilityIfNeeded();

            unsigned parametersCount = 0;
            bool isArrowFunctionParameterList = true;
            bool isMethod = false;
            isArrowFunction = parseFormalParameters(syntaxChecker, syntaxChecker.createFormalParameterList(), isArrowFunctionParameterList, isMethod, parametersCount) && consume(CLOSEPAREN) && match(ARROWFUNCTION);
            propagateError();
            popScope(fakeScope, syntaxChecker.NeedsFreeVariableInfo);
        }
        restoreSavePoint(context, saveArrowFunctionPoint);
        return isArrowFunction;
    }

    if (matchSpecIdentifier()) {
        semanticFailIfTrue(isDisallowedIdentifierAwait(m_token), "Cannot use 'await' as a parameter name ", disallowedIdentifierAwaitReason());
        SavePoint saveArrowFunctionPoint = createSavePoint(context);
        next();
        bool isArrowFunction = match(ARROWFUNCTION);
        restoreSavePoint(context, saveArrowFunctionPoint);
        return isArrowFunction;
    }

    return false;
}

template <typename LexerType>
bool Parser<LexerType>::allowAutomaticSemicolon()
{
    return match(CLOSEBRACE) || match(EOFTOK) || m_lexer->hasLineTerminatorBeforeToken();
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseSourceElements(TreeBuilder& context, SourceElementsMode mode)
{
    const unsigned lengthOfUseStrictLiteral = 12; // "use strict".length
    TreeSourceElements sourceElements = context.createSourceElements();
    const Identifier* directive = nullptr;
    unsigned directiveLiteralLength = 0;
    auto savePoint = createSavePoint(context);
    bool shouldCheckForUseStrict = mode == CheckForStrictMode;
    
    while (TreeStatement statement = parseStatementListItem(context, directive, &directiveLiteralLength)) {
        if (shouldCheckForUseStrict) {
            if (directive) {
                // "use strict" must be the exact literal without escape sequences or line continuation.
                if (directiveLiteralLength == lengthOfUseStrictLiteral && m_vm.propertyNames->useStrictIdentifier == *directive) {
                    setStrictMode();
                    shouldCheckForUseStrict = false; // We saw "use strict", there is no need to keep checking for it.
                    if (!isValidStrictMode()) {
                        if (m_parserState.lastFunctionName) {
                            semanticFailIfTrue(m_vm.propertyNames->arguments == *m_parserState.lastFunctionName, "Cannot name a function 'arguments' in strict mode");
                            semanticFailIfTrue(m_vm.propertyNames->eval == *m_parserState.lastFunctionName, "Cannot name a function 'eval' in strict mode");
                        }
                        semanticFailIfTrue(hasDeclaredVariable(m_vm.propertyNames->arguments), "Cannot declare a variable named 'arguments' in strict mode");
                        semanticFailIfTrue(hasDeclaredVariable(m_vm.propertyNames->eval), "Cannot declare a variable named 'eval' in strict mode");
                        semanticFailIfTrue(currentScope()->hasNonSimpleParameterList(), "'use strict' directive not allowed inside a function with a non-simple parameter list");
                        semanticFailIfFalse(isValidStrictMode(), "Invalid parameters or function name in strict mode");
                    }
                    // Since strict mode is changed, restoring lexer state by calling next() may cause errors.
                    restoreSavePoint(context, savePoint);
                    propagateError();
                    continue;
                }

                // We saw a directive, but it wasn't "use strict". We reset our state to
                // see if the next statement we parse is also a directive.
                directive = nullptr;
            } else {
                // We saw a statement that wasn't in the form of a directive. The spec says that "use strict"
                // is only allowed as the first statement, or after a sequence of directives before it, but
                // not after non-directive statements.
                shouldCheckForUseStrict = false;
            }
        }
        context.appendStatement(sourceElements, statement);
    }

    propagateError();
    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseModuleSourceElements(TreeBuilder& context)
{
    TreeSourceElements sourceElements = context.createSourceElements();
    SyntaxChecker syntaxChecker(const_cast<VM&>(m_vm), m_lexer.get());

    while (true) {
        TreeStatement statement = 0;
        switch (m_token.m_type) {
        case EXPORT_:
            statement = parseExportDeclaration(context);
            if (statement)
                recordPauseLocation(context.breakpointLocation(statement));
            break;

        case IMPORT: {
            SavePoint savePoint = createSavePoint(context);
            next();
            bool isImportDeclaration = !match(OPENPAREN) && !match(DOT);
            restoreSavePoint(context, savePoint);
            if (isImportDeclaration) {
                statement = parseImportDeclaration(context);
                if (statement)
                    recordPauseLocation(context.breakpointLocation(statement));
                break;
            }

            // This is `import("...")` call or `import.meta` meta property case.
            [[fallthrough]];
        }

        default: {
            const Identifier* directive = nullptr;
            unsigned directiveLiteralLength = 0;
            if (sourceParseMode() == SourceParseMode::ModuleAnalyzeMode) {
                if (!parseStatementListItem(syntaxChecker, directive, &directiveLiteralLength))
                    goto end;
                continue;
            }
            statement = parseStatementListItem(context, directive, &directiveLiteralLength);
            break;
        }
        }

        if (!statement)
            goto end;
        context.appendStatement(sourceElements, statement);
    }

end:
    propagateError();

    for (const auto& pair : m_moduleScopeData->exportedBindings()) {
        const auto& uid = pair.key;
        if (currentScope()->hasDeclaredVariable(uid.get())) {
            currentScope()->declaredVariables().markVariableAsExported(uid.get());
            continue;
        }

        if (currentScope()->hasLexicallyDeclaredVariable(uid.get())) {
            currentScope()->lexicalVariables().markVariableAsExported(uid.get());
            continue;
        }

        semanticFail("Exported binding '", uid.get(), "' needs to refer to a top-level declared variable");
    }

    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseGeneratorFunctionSourceElements(TreeBuilder& context, const Identifier& name, SourceElementsMode mode)
{
    auto sourceElements = context.createSourceElements();

    unsigned functionStart = tokenStart();
    JSTokenLocation startLocation(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    unsigned startColumn = tokenColumn();
    int functionNameStart = m_token.m_location.startOffset;
    int parametersStart = m_token.m_location.startOffset;

    ParserFunctionInfo<TreeBuilder> info;
    info.name = &m_vm.propertyNames->nullIdentifier;
    createGeneratorParameters(context, info.parameterCount);
    info.startOffset = parametersStart;
    info.startLine = tokenLine();

    {
        AutoPopScopeRef generatorBodyScope(this, pushScope());

        generatorBodyScope->setSourceParseMode(SourceParseMode::GeneratorBodyMode);
        resetImplementationVisibilityIfNeeded();

        generatorBodyScope->setConstructorKind(ConstructorKind::None);
        generatorBodyScope->setExpectedSuperBinding(m_superBinding);

        SyntaxChecker generatorFunctionContext(const_cast<VM&>(m_vm), m_lexer.get());
        failIfFalse(parseSourceElements(generatorFunctionContext, mode), "Cannot parse the body of a generator");
        popScope(generatorBodyScope, TreeBuilder::NeedsFreeVariableInfo);
    }
    info.body = context.createFunctionMetadata(startLocation, tokenLocation(), startColumn, tokenColumn(), functionStart, functionNameStart, parametersStart, implementationVisibility(), lexicallyScopedFeatures(), ConstructorKind::None, m_superBinding, info.parameterCount, SourceParseMode::GeneratorBodyMode, false);

    info.endLine = tokenLine();
    info.endOffset = m_token.m_data.offset;
    info.parametersStartColumn = startColumn;

    auto functionExpr = context.createGeneratorFunctionBody(startLocation, info, name);
    auto statement = context.createExprStatement(startLocation, functionExpr, start, m_lastTokenEndPosition.line);
    context.appendStatement(sourceElements, statement);

    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseAsyncFunctionSourceElements(TreeBuilder& context, const Identifier& calleeName, bool isArrowFunctionBodyExpression, SourceElementsMode mode)
{
    ASSERT(isAsyncFunctionOrAsyncGeneratorWrapperParseMode(sourceParseMode()));
    auto sourceElements = context.createSourceElements();

    unsigned functionStart = tokenStart();
    JSTokenLocation startLocation(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    unsigned startColumn = tokenColumn();
    int functionNameStart = m_token.m_location.startOffset;
    int parametersStart = m_token.m_location.startOffset;

    ParserFunctionInfo<TreeBuilder> info;
    info.name = &m_vm.propertyNames->nullIdentifier;
    createGeneratorParameters(context, info.parameterCount);
    info.startOffset = parametersStart;
    info.startLine = tokenLine();

    SourceParseMode parseMode = getAsyncFunctionBodyParseMode(sourceParseMode());
    SetForScope innerParseMode(m_parseMode, parseMode);
    {
        AutoPopScopeRef asyncFunctionBodyScope(this, pushScope());

        asyncFunctionBodyScope->setSourceParseMode(sourceParseMode());
        resetImplementationVisibilityIfNeeded();

        SyntaxChecker syntaxChecker(const_cast<VM&>(m_vm), m_lexer.get());
        if (isArrowFunctionBodyExpression) {
            if (m_debuggerParseData)
                failIfFalse(parseArrowFunctionSingleExpressionBodySourceElements(context), "Cannot parse the body of async arrow function");
            else
                failIfFalse(parseArrowFunctionSingleExpressionBodySourceElements(syntaxChecker), "Cannot parse the body of async arrow function");
        } else {
            if (m_debuggerParseData)
                failIfFalse(parseSourceElements(context, mode), "Cannot parse the body of async function");
            else
                failIfFalse(parseSourceElements(syntaxChecker, mode), "Cannot parse the body of async function");
        }
        popScope(asyncFunctionBodyScope, TreeBuilder::NeedsFreeVariableInfo);
    }
    info.body = context.createFunctionMetadata(startLocation, tokenLocation(), startColumn, tokenColumn(), functionStart, functionNameStart, parametersStart, implementationVisibility(), lexicallyScopedFeatures(), ConstructorKind::None, m_superBinding, info.parameterCount, sourceParseMode(), isArrowFunctionBodyExpression);

    info.endLine = tokenLine();
    info.endOffset = isArrowFunctionBodyExpression ? tokenLocation().endOffset : m_token.m_data.offset;
    info.parametersStartColumn = startColumn;

    auto functionExpr = context.createAsyncFunctionBody(startLocation, info, parseMode, calleeName);
    auto statement = context.createExprStatement(startLocation, functionExpr, start, m_lastTokenEndPosition.line);
    context.appendStatement(sourceElements, statement);

    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseAsyncGeneratorFunctionSourceElements(TreeBuilder& context, const Identifier& calleeName, bool isArrowFunctionBodyExpression, SourceElementsMode mode)
{
    ASSERT(isAsyncGeneratorWrapperParseMode(sourceParseMode()));
    auto sourceElements = context.createSourceElements();
        
    unsigned functionStart = tokenStart();
    JSTokenLocation startLocation(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    unsigned startColumn = tokenColumn();
    int functionNameStart = m_token.m_location.startOffset;
    int parametersStart = m_token.m_location.startOffset;
    
    ParserFunctionInfo<TreeBuilder> info;
    info.name = &m_vm.propertyNames->nullIdentifier;
    createGeneratorParameters(context, info.parameterCount);
    info.startOffset = parametersStart;
    info.startLine = tokenLine();

    SourceParseMode parseMode = SourceParseMode::AsyncGeneratorBodyMode;
    SetForScope innerParseMode(m_parseMode, parseMode);
    {
        AutoPopScopeRef asyncFunctionBodyScope(this, pushScope());

        asyncFunctionBodyScope->setSourceParseMode(sourceParseMode());
        resetImplementationVisibilityIfNeeded();

        SyntaxChecker syntaxChecker(const_cast<VM&>(m_vm), m_lexer.get());
        if (isArrowFunctionBodyExpression) {
            if (m_debuggerParseData)
                failIfFalse(parseArrowFunctionSingleExpressionBodySourceElements(context), "Cannot parse the body of async arrow function");
            else
                failIfFalse(parseArrowFunctionSingleExpressionBodySourceElements(syntaxChecker), "Cannot parse the body of async arrow function");
        } else {
            if (m_debuggerParseData)
                failIfFalse(parseSourceElements(context, mode), "Cannot parse the body of async function");
            else
                failIfFalse(parseSourceElements(syntaxChecker, mode), "Cannot parse the body of async function");
        }
        popScope(asyncFunctionBodyScope, TreeBuilder::NeedsFreeVariableInfo);
    }
    info.body = context.createFunctionMetadata(startLocation, tokenLocation(), startColumn, tokenColumn(), functionStart, functionNameStart, parametersStart, implementationVisibility(), lexicallyScopedFeatures(), ConstructorKind::None, m_superBinding, info.parameterCount, parseMode, isArrowFunctionBodyExpression);

    info.endLine = tokenLine();
    info.endOffset = isArrowFunctionBodyExpression ? tokenLocation().endOffset : m_token.m_data.offset;
    info.parametersStartColumn = startColumn;

    auto functionExpr = context.createAsyncFunctionBody(startLocation, info, parseMode, calleeName);
    auto statement = context.createExprStatement(startLocation, functionExpr, start, m_lastTokenEndPosition.line);
    context.appendStatement(sourceElements, statement);
        
    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseSingleFunction(TreeBuilder& context, std::optional<int> functionConstructorParametersEndPosition)
{
    TreeSourceElements sourceElements = context.createSourceElements();
    TreeStatement statement = 0;
    switch (m_token.m_type) {
    case FUNCTION:
        statement = parseFunctionDeclaration(context, FunctionDeclarationType::Declaration, ExportType::NotExported, DeclarationDefaultContext::Standard, functionConstructorParametersEndPosition);
        break;
    case IDENT:
        if (*m_token.m_data.ident == m_vm.propertyNames->async && !m_token.m_data.escaped) [[likely]] {
            unsigned functionStart = m_token.m_startPosition;
            next();
            failIfFalse(match(FUNCTION) && !m_lexer->hasLineTerminatorBeforeToken(), "Cannot parse the async function");
            statement = parseAsyncFunctionDeclaration(context, functionStart, ExportType::NotExported, DeclarationDefaultContext::Standard, functionConstructorParametersEndPosition);
            break;
        }
        [[fallthrough]];
    default:
        failDueToUnexpectedToken();
    }

    if (statement) {
        context.setEndOffset(statement, m_lastTokenEndPosition.offset);
        context.appendStatement(sourceElements, statement);
    }

    propagateError();
    return sourceElements;
}

    
template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseStatementListItem(TreeBuilder& context, const Identifier*& directive, unsigned* directiveLiteralLength)
{
    // The grammar is documented here:
    // http://www.ecma-international.org/ecma-262/6.0/index.html#sec-statements
    DepthManager statementDepth(&m_statementDepth);
    m_statementDepth++;
    failIfStackOverflow();
    TreeStatement result = 0;
    bool shouldSetEndOffset = true;
    bool shouldSetPauseLocation = false;

    switch (m_token.m_type) {
    case CONSTTOKEN:
        result = parseVariableDeclaration(context, DeclarationType::ConstDeclaration);
        shouldSetPauseLocation = true;
        break;
    case LET: {
        bool shouldParseVariableDeclaration = true;
        if (!strictMode()) {
            SavePoint savePoint = createSavePoint(context);
            next();
            // Intentionally use `matchIdentifierOrPossiblyEscapedContextualKeyword()` and not `matchSpecIdentifier()`.
            // We would like contextual keywords to fall under parseVariableDeclaration even when not used as identifiers.
            // For example, under a generator context, matchSpecIdentifier() for "yield" returns `false`.
            // But we would like to enter parseVariableDeclaration and raise an error under the context of parseVariableDeclaration
            // to raise consistent errors between "var", "const" and "let".
            if (!matchIdentifierOrPossiblyEscapedContextualKeyword() && !match(OPENBRACE) && !match(OPENBRACKET))
                shouldParseVariableDeclaration = false;
            restoreSavePoint(context, savePoint);
        }
        if (shouldParseVariableDeclaration)
            result = parseVariableDeclaration(context, DeclarationType::LetDeclaration);
        else {
            bool allowFunctionDeclarationAsStatement = true;
            result = parseExpressionOrLabelStatement(context, allowFunctionDeclarationAsStatement);
        }
        shouldSetPauseLocation = !context.shouldSkipPauseLocation(result);
        break;
    }
    case CLASSTOKEN:
        result = parseClassDeclaration(context);
        break;
    case FUNCTION:
        result = parseFunctionDeclaration(context);
        break;
    case ESCAPED_KEYWORD:
        if (!matchAllowedEscapedContextualKeyword()) [[unlikely]]
            failDueToUnexpectedToken();
        [[fallthrough]];
    case IDENT:
        if (*m_token.m_data.ident == m_vm.propertyNames->async && !m_token.m_data.escaped) [[unlikely]] {
            // Eagerly parse as AsyncFunctionDeclaration. This is the uncommon case,
            // but could be mistakenly parsed as an AsyncFunctionExpression.
            SavePoint savePoint = createSavePoint(context);
            unsigned functionStart = m_token.m_startPosition;
            next();
            if (match(FUNCTION) && !m_lexer->hasLineTerminatorBeforeToken()) [[unlikely]] {
                result = parseAsyncFunctionDeclaration(context, functionStart);
                break;
            }
            restoreSavePoint(context, savePoint);
        }
        [[fallthrough]];
    case AWAIT:
    case YIELD: {
        if (currentScope()->isStaticBlock()) [[unlikely]] {
            failIfTrue(match(YIELD), "Cannot use 'yield' within static block");
            failIfTrue(match(AWAIT), "Cannot use 'await' within static block");
        }
        // This is a convenient place to notice labeled statements
        // (even though we also parse them as normal statements)
        // because we allow the following type of code in sloppy mode:
        // ``` function foo() { label: function bar() { } } ```
        bool allowFunctionDeclarationAsStatement = true;
        result = parseExpressionOrLabelStatement(context, allowFunctionDeclarationAsStatement);
        shouldSetPauseLocation = !context.shouldSkipPauseLocation(result);
        break;
    }
    default:
        m_statementDepth--; // parseStatement() increments the depth.
        result = parseStatement(context, directive, directiveLiteralLength);
        shouldSetEndOffset = false;
        break;
    }

    if (result) {
        if (shouldSetEndOffset)
            context.setEndOffset(result, m_lastTokenEndPosition.offset);
        if (shouldSetPauseLocation)
            recordPauseLocation(context.breakpointLocation(result));
    }

    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseVariableDeclaration(TreeBuilder& context, DeclarationType declarationType, ExportType exportType)
{
    ASSERT(match(VAR) || match(LET) || match(CONSTTOKEN));
    JSTokenLocation location(tokenLocation());
    int start = tokenLine();
    int end = 0;
    int scratch;
    TreeDestructuringPattern scratch1 = 0;
    TreeExpression scratch2 = 0;
    JSTextPosition scratch3;
    bool scratchBool;
    TreeExpression variableDecls = parseVariableDeclarationList(context, scratch, scratch1, scratch2, scratch3, scratch3, scratch3, VarDeclarationContext, declarationType, exportType, scratchBool);
    propagateError();
    failIfFalse(autoSemiColon(), "Expected ';' after variable declaration");
    
    return context.createDeclarationStatement(location, variableDecls, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseDoWhileStatement(TreeBuilder& context)
{
    ASSERT(match(DO));
    int startLine = tokenLine();
    next();
    const Identifier* unused = nullptr;
    startLoop();
    TreeStatement statement = parseStatement(context, unused);
    endLoop();
    failIfFalse(statement, "Expected a statement following 'do'");
    int endLine = tokenLine();
    JSTokenLocation location(tokenLocation());
    handleProductionOrFail(WHILE, "while", "end", "do-while loop");
    handleProductionOrFail(OPENPAREN, "(", "start", "do-while loop condition");
    semanticFailIfTrue(match(CLOSEPAREN), "Must provide an expression as a do-while loop condition");
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Unable to parse do-while loop condition");
    recordPauseLocation(context.breakpointLocation(expr));
    handleProductionOrFail(CLOSEPAREN, ")", "end", "do-while loop condition");
    consume(SEMICOLON); // Always performs automatic semicolon insertion.
    return context.createDoWhileStatement(location, statement, expr, startLine, endLine);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseWhileStatement(TreeBuilder& context)
{
    ASSERT(match(WHILE));
    JSTokenLocation location(tokenLocation());
    int startLine = tokenLine();
    next();
    
    handleProductionOrFail(OPENPAREN, "(", "start", "while loop condition");
    semanticFailIfTrue(match(CLOSEPAREN), "Must provide an expression as a while loop condition");
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Unable to parse while loop condition");
    recordPauseLocation(context.breakpointLocation(expr));
    int endLine = tokenLine();
    handleProductionOrFail(CLOSEPAREN, ")", "end", "while loop condition");

    const Identifier* unused = nullptr;
    startLoop();
    TreeStatement statement = parseStatement(context, unused);
    endLoop();
    failIfFalse(statement, "Expected a statement as the body of a while loop");
    return context.createWhileStatement(location, expr, statement, startLine, endLine);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseVariableDeclarationList(TreeBuilder& context, int& declarations, TreeDestructuringPattern& lastPattern, TreeExpression& lastInitializer, JSTextPosition& identStart, JSTextPosition& initStart, JSTextPosition& initEnd, VarDeclarationListContext declarationListContext, DeclarationType declarationType, ExportType exportType, bool& forLoopConstDoesNotHaveInitializer)
{
    ASSERT(declarationType == DeclarationType::LetDeclaration || declarationType == DeclarationType::VarDeclaration || declarationType == DeclarationType::ConstDeclaration);
    TreeExpression head = 0;
    JSTokenLocation headLocation;
    TreeExpression tail = 0;
    const Identifier* lastIdent;
    JSToken lastIdentToken; 
    AssignmentContext assignmentContext = assignmentContextFromDeclarationType(declarationType);
    do {
        lastPattern = TreeDestructuringPattern(0);
        lastIdent = nullptr;
        JSTokenLocation location(tokenLocation());
        next();
        if (head) {
            // Move the location of subsequent declarations after the comma.
            location = tokenLocation();
        }
        TreeExpression node = 0;
        declarations++;
        bool hasInitializer = false;

        failIfTrue(match(PRIVATENAME), "Cannot use a private name to declare a variable");
        if (matchSpecIdentifier()) {
            semanticFailIfTrue(currentScope()->isStaticBlock() && isArgumentsIdentifier(), "Cannot use 'arguments' as an identifier in static block");
            failIfTrue(isPossiblyEscapedLet(m_token) && (declarationType == DeclarationType::LetDeclaration || declarationType == DeclarationType::ConstDeclaration), 
                "Cannot use 'let' as an identifier name for a LexicalDeclaration");
            semanticFailIfTrue(isDisallowedIdentifierAwait(m_token), "Cannot use 'await' as a ", declarationTypeToVariableKind(declarationType), " ", disallowedIdentifierAwaitReason());
            JSTextPosition varStart = tokenStartPosition();
            JSTokenLocation varStartLocation(tokenLocation());
            identStart = varStart;
            const Identifier* name = m_token.m_data.ident;
            lastIdent = name;
            lastIdentToken = m_token;
            next();
            hasInitializer = match(EQUAL);
            DeclarationResultMask declarationResult = declareVariable(name, declarationType);
            if (declarationResult != DeclarationResult::Valid) {
                failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare a variable named ", name->impl(), " in strict mode");
                if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration) [[unlikely]] {
                    semanticFailIfTrue(declarationType == DeclarationType::LetDeclaration, "Cannot declare a let variable twice: '", name->impl(), "'");
                    semanticFailIfTrue(declarationType == DeclarationType::ConstDeclaration, "Cannot declare a const variable twice: '", name->impl(), "'");
                    ASSERT(declarationType == DeclarationType::VarDeclaration);
                    semanticFail("Cannot declare a var variable that shadows a let/const/class variable: '", name->impl(), "'");
                }
            }
            if (exportType == ExportType::Exported) {
                semanticFailIfFalse(exportName(*name), "Cannot export a duplicate name '", name->impl(), "'");
                m_moduleScopeData->exportBinding(*name);
            }

            if (hasInitializer) {
                JSTextPosition varDivot = tokenStartPosition() + 1;
                initStart = tokenStartPosition();
                next(TreeBuilder::DontBuildStrings); // consume '='
                propagateError();
                TreeExpression initializer = parseAssignmentExpression(context);
                initEnd = lastTokenEndPosition();
                lastInitializer = initializer;
                failIfFalse(initializer, "Expected expression as the intializer for the variable '", name->impl(), "'");
                
                node = context.createAssignResolve(location, *name, initializer, varStart, varDivot, lastTokenEndPosition(), assignmentContext);
            } else {
                if (declarationListContext == ForLoopContext && declarationType == DeclarationType::ConstDeclaration)
                    forLoopConstDoesNotHaveInitializer = true;
                failIfTrue(declarationListContext != ForLoopContext && declarationType == DeclarationType::ConstDeclaration, "const declared variable '", name->impl(), "'", " must have an initializer");
                if (declarationType == DeclarationType::VarDeclaration)
                    node = context.createEmptyVarExpression(varStartLocation, *name);
                else
                    node = context.createEmptyLetExpression(varStartLocation, *name);
            }
        } else {
            lastIdent = nullptr;
            TreeDestructuringPattern pattern;
            {
                bool allowsInOperator = true;
                SetForScope allowsInScope(m_allowsIn, allowsInOperator);
                pattern = parseDestructuringPattern(context, destructuringKindFromDeclarationType(declarationType), exportType, nullptr, nullptr, assignmentContext);
            }
            failIfFalse(pattern, "Cannot parse this destructuring pattern");
            hasInitializer = match(EQUAL);
            failIfTrue(declarationListContext == VarDeclarationContext && !hasInitializer, "Expected an initializer in destructuring variable declaration");
            lastPattern = pattern;
            if (hasInitializer) {
                next(TreeBuilder::DontBuildStrings); // consume '='
                TreeExpression rhs = parseAssignmentExpression(context);
                propagateError();
                ASSERT(rhs);
                node = context.createDestructuringAssignment(location, pattern, rhs);
                lastInitializer = rhs;
            }
        }

        if (node) {
            if (!head) {
                head = node;
                headLocation = location;
            } else {
                if (!tail) {
                    recordPauseLocation(context.breakpointLocation(head));
                    head = tail = context.createCommaExpr(headLocation, head);
                }
                recordPauseLocation(context.breakpointLocation(node));
                tail = context.appendToCommaExpr(location, tail, node);
            }
        }
    } while (match(COMMA));
    if (lastIdent)
        lastPattern = context.createBindingLocation(lastIdentToken.m_location, *lastIdent, lastIdentToken.m_startPosition, lastIdentToken.m_endPosition, assignmentContext);

    return head;
}

template <typename LexerType>
bool Parser<LexerType>::declareRestOrNormalParameter(const Identifier& name, const Identifier** duplicateIdentifier)
{
    DeclarationResultMask declarationResult = declareParameter(&name);
    if ((declarationResult & DeclarationResult::InvalidStrictMode) && strictMode()) [[unlikely]] {
        semanticFailIfTrue(isEvalOrArguments(&name), "Cannot destructure to a parameter name '", name.impl(), "' in strict mode");
        if (m_parserState.lastFunctionName && name == *m_parserState.lastFunctionName)
            semanticFail("Cannot declare a parameter named '", name.impl(), "' as it shadows the name of a strict mode function");
        semanticFailureDueToKeyword("parameter name");
        if (!m_lexer->isReparsingFunction() && hasDeclaredParameter(name))
            semanticFail("Cannot declare a parameter named '", name.impl(), "' in strict mode as it has already been declared");
        semanticFail("Cannot declare a parameter named '", name.impl(), "' in strict mode");
    }
    if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration) {
        // It's not always an error to define a duplicate parameter.
        // It's only an error when there are default parameter values or destructuring parameters.
        // We note this value now so we can check it later.
        if (duplicateIdentifier)
            *duplicateIdentifier = &name;
    }

    return true;
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::createBindingPattern(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier& name, const JSToken& token, AssignmentContext bindingContext, const Identifier** duplicateIdentifier)
{
    ASSERT(!name.isNull());
    
    ASSERT(name.impl()->isAtom() || name.impl()->isSymbol());

    switch (kind) {
    case DestructuringKind::DestructureToVariables: {
        DeclarationResultMask declarationResult = declareVariable(&name);
        failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare a variable named '", name.impl(), "' in strict mode");
        semanticFailIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare a var variable that shadows a let/const/class variable: '", name.impl(), "'");
        break;
    }

    case DestructuringKind::DestructureToLet:
    case DestructuringKind::DestructureToConst:
    case DestructuringKind::DestructureToCatchParameters: {
        DeclarationResultMask declarationResult = declareVariable(&name, kind == DestructuringKind::DestructureToConst ? DeclarationType::ConstDeclaration : DeclarationType::LetDeclaration);
        if (declarationResult != DeclarationResult::Valid) {
            failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot destructure to a variable named '", name.impl(), "' in strict mode");
            failIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare a lexical variable twice: '", name.impl(), "'");
        }
        break;
    }

    case DestructuringKind::DestructureToParameters: {
        declareRestOrNormalParameter(name, duplicateIdentifier);
        propagateError();
        break;
    }

    case DestructuringKind::DestructureToExpressions: {
        break;
    }
    }

    if (exportType == ExportType::Exported) {
        semanticFailIfFalse(exportName(name), "Cannot export a duplicate name '", name.impl(), "'");
        m_moduleScopeData->exportBinding(name);
    }
    return context.createBindingLocation(token.m_location, name, token.m_startPosition, token.m_endPosition, bindingContext);
}

template <typename LexerType>
template <class TreeBuilder> NEVER_INLINE TreeDestructuringPattern Parser<LexerType>::createAssignmentElement(TreeBuilder& context, TreeExpression& assignmentTarget, const JSTextPosition& startPosition, const JSTextPosition& endPosition)
{
    return context.createAssignmentElement(assignmentTarget, startPosition, endPosition);
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseArrowFunctionSingleExpressionBodySourceElements(TreeBuilder& context)
{
    ASSERT(!match(OPENBRACE));

    JSTokenLocation location(tokenLocation());
    JSTextPosition start = tokenStartPosition();

    failIfStackOverflow();
    TreeExpression expr = parseAssignmentExpression(context);
    failIfFalse(expr, "Cannot parse the arrow function expression");
    
    context.setEndOffset(expr, m_lastTokenEndPosition.offset);

    JSTextPosition end = tokenEndPosition();
    
    TreeSourceElements sourceElements = context.createSourceElements();
    TreeStatement body = context.createReturnStatement(location, expr, start, end);
    context.setEndOffset(body, m_lastTokenEndPosition.offset);
    recordPauseLocation(context.breakpointLocation(body));
    context.appendStatement(sourceElements, body);

    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::tryParseDestructuringPatternExpression(TreeBuilder& context, AssignmentContext bindingContext)
{
    return parseDestructuringPattern(context, DestructuringKind::DestructureToExpressions, ExportType::NotExported, nullptr, nullptr, bindingContext);
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseBindingOrAssignmentElement(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier** duplicateIdentifier, bool* hasDestructuringPattern, AssignmentContext bindingContext, int depth)
{
    if (kind == DestructuringKind::DestructureToExpressions)
        return parseAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth);
    return parseDestructuringPattern(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth);
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseObjectRestAssignmentElement(TreeBuilder& context)
{
    JSTextPosition startPosition = tokenStartPosition();
    auto element = parseMemberExpression(context);

    if (!element || !context.isAssignmentLocation(element)) [[unlikely]] {
        reclassifyExpressionError(ErrorIndicatesPattern, ErrorIndicatesNothing);
        semanticFail("Invalid destructuring assignment target");
    }

    if (strictMode() && m_parserState.lastIdentifier && context.isResolve(element)) {
        bool isEvalOrArguments = m_vm.propertyNames->eval == *m_parserState.lastIdentifier || m_vm.propertyNames->arguments == *m_parserState.lastIdentifier;
        if (isEvalOrArguments && strictMode())
            reclassifyExpressionError(ErrorIndicatesPattern, ErrorIndicatesNothing);
        failIfTrueIfStrict(isEvalOrArguments, "Cannot modify '", m_parserState.lastIdentifier->impl(), "' in strict mode");
    }

    return createAssignmentElement(context, element, startPosition, lastTokenEndPosition());
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseAssignmentElement(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier** duplicateIdentifier, bool* hasDestructuringPattern, AssignmentContext bindingContext, int depth)
{
    TreeDestructuringPattern assignmentTarget = 0;

    if (match(OPENBRACE) || match(OPENBRACKET)) {
        SavePoint savePoint = createSavePoint(context);
        assignmentTarget = parseDestructuringPattern(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth);
        if (assignmentTarget && !match(DOT) && !match(OPENBRACKET) && !match(OPENPAREN) && !match(BACKQUOTE))
            return assignmentTarget;
        restoreSavePoint(context, savePoint);
    }

    JSTextPosition startPosition = tokenStartPosition();
    auto element = parseMemberExpression(context);

    semanticFailIfFalse(element && context.isAssignmentLocation(element), "Invalid destructuring assignment target");

    if (strictMode() && m_parserState.lastIdentifier && context.isResolve(element)) {
        bool isEvalOrArguments = m_vm.propertyNames->eval == *m_parserState.lastIdentifier || m_vm.propertyNames->arguments == *m_parserState.lastIdentifier;
        failIfTrueIfStrict(isEvalOrArguments, "Cannot modify '", m_parserState.lastIdentifier->impl(), "' in strict mode");
    }

    return createAssignmentElement(context, element, startPosition, lastTokenEndPosition());
}

static const char* destructuringKindToVariableKindName(DestructuringKind kind)
{
    switch (kind) {
    case DestructuringKind::DestructureToLet:
    case DestructuringKind::DestructureToConst:
        return "lexical variable name";
    case DestructuringKind::DestructureToVariables:
        return "variable name";
    case DestructuringKind::DestructureToParameters:
        return "parameter name";
    case DestructuringKind::DestructureToCatchParameters:
        return "catch parameter name";
    case DestructuringKind::DestructureToExpressions:
        return "expression name";
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "invalid";
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseObjectRestElement(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier** duplicateIdentifier, AssignmentContext bindingContext)
{
    ASSERT(kind != DestructuringKind::DestructureToExpressions);
    failIfStackOverflow();
    TreeDestructuringPattern pattern;
    
    if (!matchSpecIdentifier()) [[unlikely]] {
        semanticFailureDueToKeyword(destructuringKindToVariableKindName(kind));
        failWithMessage("Expected a binding element");
    }
    failIfTrue(match(LET) && (kind == DestructuringKind::DestructureToLet || kind == DestructuringKind::DestructureToConst), "Cannot use 'let' as an identifier name for a LexicalDeclaration");
    semanticFailIfTrue(isDisallowedIdentifierAwait(m_token), "Cannot use 'await' as a ", destructuringKindToVariableKindName(kind), " ", disallowedIdentifierAwaitReason());
    pattern = createBindingPattern(context, kind, exportType, *m_token.m_data.ident, m_token, bindingContext, duplicateIdentifier);
    next();
    return pattern;
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseObjectRestBindingOrAssignmentElement(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier** duplicateIdentifier, AssignmentContext bindingContext)
{
    if (kind == DestructuringKind::DestructureToExpressions)
        return parseObjectRestAssignmentElement(context);
    return parseObjectRestElement(context, kind, exportType, duplicateIdentifier, bindingContext);
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseDestructuringPattern(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier** duplicateIdentifier, bool* hasDestructuringPattern, AssignmentContext bindingContext, int depth)
{
    failIfStackOverflow();
    m_parserState.assignmentCount++;
    SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
    TreeDestructuringPattern pattern;
    switch (m_token.m_type) {
    case OPENBRACKET: {
        JSTextPosition divotStart = tokenStartPosition();
        auto arrayPattern = context.createArrayPattern(m_token.m_location);
        next();

        if (hasDestructuringPattern)
            *hasDestructuringPattern = true;

        bool restElementWasFound = false;

        do {
            while (match(COMMA)) {
                context.appendArrayPatternSkipEntry(arrayPattern, m_token.m_location);
                next();
            }
            propagateError();

            if (match(CLOSEBRACKET))
                break;

            if (match(DOTDOTDOT)) [[unlikely]] {
                JSTokenLocation location = m_token.m_location;
                next();
                auto innerPattern = parseBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth + 1);
                if (kind == DestructuringKind::DestructureToExpressions && !innerPattern)
                    return 0;
                failIfFalse(innerPattern, "Cannot parse this destructuring pattern");
                context.appendArrayPatternRestEntry(arrayPattern, location, innerPattern);
                restElementWasFound = true;
                break;
            }

            JSTokenLocation location = m_token.m_location;
            auto innerPattern = parseBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth + 1);
            if (kind == DestructuringKind::DestructureToExpressions && !innerPattern)
                return 0;
            failIfFalse(innerPattern, "Cannot parse this destructuring pattern");
            TreeExpression defaultValue = parseDefaultValueForDestructuringPattern(context);
            propagateError();
            context.appendArrayPatternEntry(arrayPattern, location, innerPattern, defaultValue);
        } while (consume(COMMA));

        consumeOrFail(CLOSEBRACKET, restElementWasFound ? "Expected a closing ']' following a rest element destructuring pattern" : "Expected either a closing ']' or a ',' following an element destructuring pattern");
        context.finishArrayPattern(arrayPattern, divotStart, divotStart, lastTokenEndPosition());
        pattern = arrayPattern;
        break;
    }
    case OPENBRACE: {
        JSTextPosition divotStart = tokenStartPosition();
        auto objectPattern = context.createObjectPattern(m_token.m_location);
        next();

        if (hasDestructuringPattern)
            *hasDestructuringPattern = true;

        bool restElementWasFound = false;

        do {
            bool wasString = false;

            if (match(CLOSEBRACE))
                break;

            if (match(DOTDOTDOT)) {
                JSTokenLocation location = m_token.m_location;
                next();
                auto innerPattern = parseObjectRestBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, bindingContext);
                propagateError();
                if (!innerPattern)
                    return 0;
                context.appendObjectPatternRestEntry(m_vm, objectPattern, location, innerPattern);
                restElementWasFound = true;
                context.setContainsObjectRestElement(objectPattern, restElementWasFound);
                break;
            }

            const Identifier* propertyName = nullptr;
            TreeExpression propertyExpression = 0;
            TreeDestructuringPattern innerPattern = 0;
            JSTokenLocation location = m_token.m_location;
            bool escapedKeyword = match(ESCAPED_KEYWORD);
            if (escapedKeyword || matchSpecIdentifier()) {
                bool letMatched = match(LET);
                propertyName = m_token.m_data.ident;
                JSToken identifierToken = m_token;
                next();
                if (consume(COLON))
                    innerPattern = parseBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth + 1);
                else {
                    semanticFailIfTrue(letMatched && (kind == DestructuringKind::DestructureToLet || kind == DestructuringKind::DestructureToConst), "Cannot use the keyword 'let' as a lexical variable name");
                    semanticFailIfTrue(escapedKeyword, "Cannot use abbreviated destructuring syntax for keyword '", propertyName->impl(), "'");
                    semanticFailIfTrue(isDisallowedIdentifierAwait(identifierToken), "Cannot use 'await' as a ", destructuringKindToVariableKindName(kind), " ", disallowedIdentifierAwaitReason());
                    if (kind == DestructuringKind::DestructureToExpressions) {
                        bool isEvalOrArguments = m_vm.propertyNames->eval == *propertyName || m_vm.propertyNames->arguments == *propertyName;
                        if (isEvalOrArguments && strictMode())
                            reclassifyExpressionError(ErrorIndicatesPattern, ErrorIndicatesNothing);
                        failIfTrueIfStrict(isEvalOrArguments, "Cannot modify '", propertyName->impl(), "' in strict mode");

                        if (match(EQUAL))
                            currentScope()->useVariable(propertyName, m_vm.propertyNames->eval == *propertyName);
                    }
                    innerPattern = createBindingPattern(context, kind, exportType, *propertyName, identifierToken, bindingContext, duplicateIdentifier);
                }
            } else {
                JSTokenType tokenType = m_token.m_type;
                switch (m_token.m_type) {
                case DOUBLE:
                case INTEGER:
                    propertyName = &m_parserArena.identifierArena().makeNumericIdentifier(const_cast<VM&>(m_vm), m_token.m_data.doubleValue);
                    break;
                case STRING:
                    propertyName = m_token.m_data.ident;
                    wasString = true;
                    break;
                case BIGINT:
                    propertyName = m_parserArena.identifierArena().makeBigIntDecimalIdentifier(const_cast<VM&>(m_vm), *m_token.m_data.bigIntString, m_token.m_data.radix);
                    failIfFalse(propertyName, "Cannot parse big int property name");
                    break;
                case OPENBRACKET:
                    next();
                    propertyExpression = parseAssignmentExpression(context);
                    failIfFalse(propertyExpression, "Cannot parse computed property name");
                    matchOrFail(CLOSEBRACKET, "Expected ']' to end end a computed property name");
                    break;
                default:
                    if (m_token.m_type != RESERVED && m_token.m_type != RESERVED_IF_STRICT && !(m_token.m_type & KeywordTokenFlag)) {
                        if (kind == DestructuringKind::DestructureToExpressions) [[likely]]
                            return 0;
                        failWithMessage("Expected a property name");
                    }
                    propertyName = m_token.m_data.ident;
                    break;
                }
                next();
                if (!consume(COLON)) {
                    if (kind == DestructuringKind::DestructureToExpressions) [[likely]]
                        return 0;
                    semanticFailIfTrue(tokenType == RESERVED, "Cannot use abbreviated destructuring syntax for reserved name '", propertyName->impl(), "'");
                    semanticFailIfTrue(tokenType == RESERVED_IF_STRICT, "Cannot use abbreviated destructuring syntax for reserved name '", propertyName->impl(), "' in strict mode");
                    semanticFailIfTrue(tokenType & KeywordTokenFlag, "Cannot use abbreviated destructuring syntax for keyword '", propertyName->impl(), "'");
                    failWithMessage("Expected a ':' prior to a named destructuring property");
                }
                innerPattern = parseBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth + 1);
            }
            if (kind == DestructuringKind::DestructureToExpressions && !innerPattern)
                return 0;
            failIfFalse(innerPattern, "Cannot parse this destructuring pattern");
            TreeExpression defaultValue = parseDefaultValueForDestructuringPattern(context);
            propagateError();
            if (propertyExpression) {
                context.appendObjectPatternEntry(m_vm, objectPattern, location, propertyExpression, innerPattern, defaultValue);
                context.setContainsComputedProperty(objectPattern, true);
            } else {
                ASSERT(propertyName);
                context.appendObjectPatternEntry(objectPattern, location, wasString, *propertyName, innerPattern, defaultValue);
            }
        } while (consume(COMMA));

        if (kind == DestructuringKind::DestructureToExpressions && !match(CLOSEBRACE))
            return 0;
        consumeOrFail(CLOSEBRACE, restElementWasFound ? "Expected a closing '}' following a rest element destructuring pattern" : "Expected either a closing '}' or an ',' after a property destructuring pattern");
        context.finishObjectPattern(objectPattern, divotStart, divotStart, lastTokenEndPosition());
        pattern = objectPattern;
        break;
    }

    default: {
        if (!matchSpecIdentifier()) {
            if (kind == DestructuringKind::DestructureToExpressions) [[likely]]
                return 0;
            semanticFailureDueToKeyword(destructuringKindToVariableKindName(kind));
            failIfTrue(kind != DestructuringKind::DestructureToParameters && match(PRIVATENAME), "Cannot use a private name as a ", destructuringKindToVariableKindName(kind));
            failWithMessage("Expected a parameter pattern or a ')' in parameter list");
        }
        failIfTrue(match(LET) && (kind == DestructuringKind::DestructureToLet || kind == DestructuringKind::DestructureToConst), "Cannot use 'let' as an identifier name for a LexicalDeclaration");
        semanticFailIfTrue(isDisallowedIdentifierAwait(m_token), "Cannot use 'await' as a ", destructuringKindToVariableKindName(kind), " ", disallowedIdentifierAwaitReason());
        pattern = createBindingPattern(context, kind, exportType, *m_token.m_data.ident, m_token, bindingContext, duplicateIdentifier);
        next();
        break;
    }
    }
    return pattern;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseDefaultValueForDestructuringPattern(TreeBuilder& context)
{
    if (!match(EQUAL))
        return 0;

    next(TreeBuilder::DontBuildStrings); // consume '='
    return parseAssignmentExpression(context);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseForStatement(TreeBuilder& context)
{
    ASSERT(match(FOR));
    JSTokenLocation location(tokenLocation());
    int startLine = tokenLine();
    bool isAwaitFor = false;
    next();

    DepthManager statementDepth(&m_statementDepth);
    m_statementDepth++;

    if (match(AWAIT)) {
        semanticFailIfFalse(currentScope()->isAsyncFunction() || isModuleParseMode(sourceParseMode()), "for-await-of can only be used in an async function or async generator");
        isAwaitFor = true;
        next();
    }

    handleProductionOrFail(OPENPAREN, "(", "start", "for-loop header");
    int nonLHSCount = m_parserState.nonLHSCount;
    int declarations = 0;
    JSTokenLocation declLocation(tokenLocation());
    JSTextPosition declsStart = tokenStartPosition();
    TreeExpression decls = 0;
    TreeDestructuringPattern pattern = 0;
    bool isVarDeclaration = match(VAR);
    bool isLetDeclaration = match(LET);
    bool isConstDeclaration = match(CONSTTOKEN);
    bool forLoopConstDoesNotHaveInitializer = false;
    bool forLoopinitializerContainsClosure = false;

    AutoCleanupLexicalScope lexicalScope;

    auto popLexicalScopeIfNecessary = [&]() -> VariableEnvironment {
        if (isLetDeclaration || isConstDeclaration) {
            auto [lexicalVariables, functionDeclarations] = popScope(lexicalScope, TreeBuilder::NeedsFreeVariableInfo);
            return lexicalVariables;
        }
        return { };
    };

    if (isVarDeclaration || isLetDeclaration || isConstDeclaration) {
        /*
         for (var/let/const IDENT in/of expression) statement
         for (var/let/const varDeclarationList; expressionOpt; expressionOpt)
         */
        if (isLetDeclaration || isConstDeclaration) {
            ScopeRef newScope = pushScope();
            newScope->setIsLexicalScope();
            newScope->preventVarDeclarations();
            lexicalScope.setIsValid(newScope, this);
        }

        TreeDestructuringPattern forInTarget = 0;
        TreeExpression forInInitializer = 0;
        m_allowsIn = false;
        JSTextPosition initStart;
        JSTextPosition initEnd;
        DeclarationType declarationType;
        if (isVarDeclaration)
            declarationType = DeclarationType::VarDeclaration;
        else if (isLetDeclaration)
            declarationType = DeclarationType::LetDeclaration;
        else if (isConstDeclaration)
            declarationType = DeclarationType::ConstDeclaration;
        else
            RELEASE_ASSERT_NOT_REACHED();
        unsigned candidateCountBeforeInitializer = currentScope()->closedVariableCandidates().size();
        decls = parseVariableDeclarationList(context, declarations, forInTarget, forInInitializer, declsStart, initStart, initEnd, ForLoopContext, declarationType, ExportType::NotExported, forLoopConstDoesNotHaveInitializer);
        forLoopinitializerContainsClosure = currentScope()->closedVariableCandidates().size() > candidateCountBeforeInitializer;
        m_allowsIn = true;
        propagateError();

        // Remainder of a standard for loop is handled identically
        if (match(SEMICOLON))
            goto standardForLoop;

        failIfFalse(declarations == 1, "can only declare a single variable in an enumeration");

        // Handle for-in with var declaration
        JSTextPosition inLocation = tokenStartPosition();
        bool isOfEnumeration = false;
        if (!match(INTOKEN)) {
            failIfFalse(matchContextualKeyword(m_vm.propertyNames->of), "Expected either 'in' or 'of' in enumeration syntax");
            isOfEnumeration = true;
            next();
        } else {
            failIfFalse(!isAwaitFor, "Expected 'of' in for-await syntax");
            next();
        }

        bool hasAnyAssignments = !!forInInitializer;
        if (hasAnyAssignments) {
            semanticFailIfTrue(isOfEnumeration, "Cannot assign to the loop variable inside a for-of loop header");
            semanticFailIfTrue(strictMode() || (isLetDeclaration || isConstDeclaration) || !context.isBindingNode(forInTarget), "Cannot assign to the loop variable inside a for-in loop header");
        }

        // While for-in uses Expression, for-of / for-await-of use AssignmentExpression.
        // https://tc39.es/ecma262/#sec-for-in-and-for-of-statements
        TreeExpression expr = 0;
        if (isOfEnumeration)
            expr = parseAssignmentExpression(context);
        else
            expr = parseExpression(context);
        failIfFalse(expr, "Expected expression to enumerate");
        recordPauseLocation(context.breakpointLocation(expr));
        JSTextPosition exprEnd = lastTokenEndPosition();
        
        int endLine = tokenLine();
        
        handleProductionOrFail(CLOSEPAREN, ")", "end", (isOfEnumeration ? "for-of header" : "for-in header"));
        
        const Identifier* unused = nullptr;
        startLoop();
        TreeStatement statement = parseStatement(context, unused);
        endLoop();
        failIfFalse(statement, "Expected statement as body of for-", isOfEnumeration ? "of" : "in", " statement");
        VariableEnvironment lexicalVariables = popLexicalScopeIfNecessary();
        if (isOfEnumeration)
            return context.createForOfLoop(isAwaitFor, location, forInTarget, expr, statement, declLocation, declsStart, inLocation, exprEnd, startLine, endLine, WTFMove(lexicalVariables));
        ASSERT(!isAwaitFor);
        if (isVarDeclaration && forInInitializer)
            return context.createForInLoop(location, decls, expr, statement, declLocation, declsStart, inLocation, exprEnd, startLine, endLine, WTFMove(lexicalVariables));
        return context.createForInLoop(location, forInTarget, expr, statement, declLocation, declsStart, inLocation, exprEnd, startLine, endLine, WTFMove(lexicalVariables));
    }
    
    if (!match(SEMICOLON)) {
        if (match(OPENBRACE) || match(OPENBRACKET)) {
            SavePoint savePoint = createSavePoint(context);
            pattern = tryParseDestructuringPatternExpression(context, AssignmentContext::AssignmentExpression);
            if (pattern && (match(INTOKEN) || matchContextualKeyword(m_vm.propertyNames->of)))
                goto enumerationLoop;
            pattern = TreeDestructuringPattern(0);
            restoreSavePoint(context, savePoint);
        }
        m_allowsIn = false;
        decls = parseExpression(context);
        m_allowsIn = true;
        failIfFalse(decls, "Cannot parse for loop declarations");
        recordPauseLocation(context.breakpointLocation(decls));
    }
    
    if (match(SEMICOLON)) {
    standardForLoop:
        failIfFalse(!isAwaitFor, "Unexpected a ';' in for-await-of header");
        // Standard for loop
        if (decls)
            recordPauseLocation(context.breakpointLocation(decls));
        next();
        TreeExpression condition = 0;
        failIfTrue(forLoopConstDoesNotHaveInitializer && isConstDeclaration, "const variables in for loops must have initializers");
        
        if (!match(SEMICOLON)) {
            condition = parseExpression(context);
            failIfFalse(condition, "Cannot parse for loop condition expression");
            recordPauseLocation(context.breakpointLocation(condition));
        }
        consumeOrFail(SEMICOLON, "Expected a ';' after the for loop condition expression");
        
        TreeExpression increment = 0;
        if (!match(CLOSEPAREN)) {
            increment = parseExpression(context);
            failIfFalse(increment, "Cannot parse for loop iteration expression");
            recordPauseLocation(context.breakpointLocation(increment));
        }
        int endLine = tokenLine();
        handleProductionOrFail(CLOSEPAREN, ")", "end", "for-loop header");
        const Identifier* unused = nullptr;
        startLoop();
        TreeStatement statement = parseStatement(context, unused);
        endLoop();
        failIfFalse(statement, "Expected a statement as the body of a for loop");
        VariableEnvironment lexicalVariables = popLexicalScopeIfNecessary();
        return context.createForLoop(location, decls, condition, increment, statement, startLine, endLine, WTFMove(lexicalVariables), forLoopinitializerContainsClosure);
    }
    
    // For-in and For-of loop
enumerationLoop:
    failIfFalse(nonLHSCount == m_parserState.nonLHSCount, "Expected a reference on the left hand side of an enumeration statement");
    bool isOfEnumeration = false;
    JSTextPosition inLocation = tokenStartPosition();
    if (!match(INTOKEN)) {
        failIfFalse(matchContextualKeyword(m_vm.propertyNames->of), "Expected either 'in' or 'of' in enumeration syntax");
        isOfEnumeration = true;
        next();
    } else {
        failIfFalse(!isAwaitFor, "Expected 'of' in for-await syntax");
        next();
    }

    // While for-in uses Expression, for-of / for-await-of use AssignmentExpression.
    // https://tc39.es/ecma262/#sec-for-in-and-for-of-statements
    TreeExpression expr = 0;
    if (isOfEnumeration)
        expr = parseAssignmentExpression(context);
    else
        expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse subject for-", isOfEnumeration ? "of" : "in", " statement");
    recordPauseLocation(context.breakpointLocation(expr));
    JSTextPosition exprEnd = lastTokenEndPosition();
    int endLine = tokenLine();
    
    handleProductionOrFail(CLOSEPAREN, ")", "end", (isOfEnumeration ? "for-of header" : "for-in header"));
    const Identifier* unused = nullptr;
    startLoop();
    TreeStatement statement = parseStatement(context, unused);
    endLoop();
    failIfFalse(statement, "Expected a statement as the body of a for-", isOfEnumeration ? "of" : "in", " loop");
    if (pattern) {
        ASSERT(!decls);
        VariableEnvironment lexicalVariables = popLexicalScopeIfNecessary();
        if (isOfEnumeration)
            return context.createForOfLoop(isAwaitFor, location, pattern, expr, statement, declLocation, declsStart, inLocation, exprEnd, startLine, endLine, WTFMove(lexicalVariables));
        ASSERT(!isAwaitFor);
        return context.createForInLoop(location, pattern, expr, statement, declLocation, declsStart, inLocation, exprEnd, startLine, endLine, WTFMove(lexicalVariables));
    }

    semanticFailIfFalse(isSimpleAssignmentTarget(context, decls), "Left side of assignment is not a reference");

    VariableEnvironment lexicalVariables = popLexicalScopeIfNecessary();
    if (isOfEnumeration)
        return context.createForOfLoop(isAwaitFor, location, decls, expr, statement, declLocation, declsStart, inLocation, exprEnd, startLine, endLine, WTFMove(lexicalVariables));
    ASSERT(!isAwaitFor);
    return context.createForInLoop(location, decls, expr, statement, declLocation, declsStart, inLocation, exprEnd, startLine, endLine, WTFMove(lexicalVariables));
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseBreakStatement(TreeBuilder& context)
{
    ASSERT(match(BREAK));
    JSTokenLocation location(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    JSTextPosition end = tokenEndPosition();
    next();

    std::optional<bool> isBreakValid;
    if (currentScope()->isStaticBlock()) [[unlikely]] {
        isBreakValid = breakIsValid();
        semanticFailIfTrue(!currentScope()->breakIsValid() && !isBreakValid.value(), "'break' cannot cross static block boundary");
    }

    if (autoSemiColon()) {
        semanticFailIfFalse(isBreakValid.value_or(breakIsValid()), "'break' is only valid inside a switch or loop statement");
        return context.createBreakStatement(location, &m_vm.propertyNames->nullIdentifier, start, end);
    }
    failIfFalse(matchSpecIdentifier(), "Expected an identifier as the target for a break statement");
    const Identifier* ident = m_token.m_data.ident;
    semanticFailIfFalse(getLabel(ident), "Cannot use the undeclared label '", ident->impl(), "'");
    end = tokenEndPosition();
    next();
    failIfFalse(autoSemiColon(), "Expected a ';' following a targeted break statement");
    return context.createBreakStatement(location, ident, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseContinueStatement(TreeBuilder& context)
{
    ASSERT(match(CONTINUE));
    JSTokenLocation location(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    JSTextPosition end = tokenEndPosition();
    next();

    std::optional<bool> isContinueValid;
    if (currentScope()->isStaticBlock()) [[unlikely]] {
        isContinueValid = continueIsValid();
        semanticFailIfTrue(!currentScope()->continueIsValid() && !isContinueValid.value(), "'continue' cannot cross static block boundary");
    }

    if (autoSemiColon()) {
        semanticFailIfFalse(isContinueValid.value_or(continueIsValid()), "'continue' is only valid inside a loop statement");
        return context.createContinueStatement(location, &m_vm.propertyNames->nullIdentifier, start, end);
    }
    failIfFalse(matchSpecIdentifier(), "Expected an identifier as the target for a continue statement");
    const Identifier* ident = m_token.m_data.ident;
    ScopeLabelInfo* label = getLabel(ident);
    semanticFailIfFalse(label, "Cannot use the undeclared label '", ident->impl(), "'");
    semanticFailIfFalse(label->isLoop, "Cannot continue to the label '", ident->impl(), "' as it is not targeting a loop");
    end = tokenEndPosition();
    next();
    failIfFalse(autoSemiColon(), "Expected a ';' following a targeted continue statement");
    return context.createContinueStatement(location, ident, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseReturnStatement(TreeBuilder& context)
{
    ASSERT(match(RETURN));
    m_parserState.returnStatementCount++;
    JSTokenLocation location(tokenLocation());
    semanticFailIfFalse(currentScope()->isFunction() && !currentScope()->isStaticBlock(), "Return statements are only valid inside functions");
    JSTextPosition start = tokenStartPosition();
    JSTextPosition end = tokenEndPosition();
    next();
    // We do the auto semicolon check before attempting to parse expression
    // as we need to ensure the a line break after the return correctly terminates
    // the statement
    if (match(SEMICOLON))
        end = tokenEndPosition();

    if (autoSemiColon())
        return context.createReturnStatement(location, 0, start, end);
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse the return expression");
    end = lastTokenEndPosition();
    if (match(SEMICOLON))
        end  = tokenEndPosition();
    failIfFalse(autoSemiColon(), "Expected a ';' following a return statement");
    return context.createReturnStatement(location, expr, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseThrowStatement(TreeBuilder& context)
{
    ASSERT(match(THROW));
    JSTokenLocation location(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    next();
    failIfTrue(match(SEMICOLON), "Expected expression after 'throw'");
    semanticFailIfTrue(autoSemiColon(), "Cannot have a newline after 'throw'");
    
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse expression for throw statement");
    JSTextPosition end = lastTokenEndPosition();
    failIfFalse(autoSemiColon(), "Expected a ';' after a throw statement");
    
    return context.createThrowStatement(location, expr, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseWithStatement(TreeBuilder& context)
{
    ASSERT(match(WITH));
    JSTokenLocation location(tokenLocation());
    semanticFailIfTrue(strictMode(), "'with' statements are not valid in strict mode");
    currentScope()->setNeedsFullActivation();
    int startLine = tokenLine();
    next();

    handleProductionOrFail(OPENPAREN, "(", "start", "subject of a 'with' statement");
    int start = tokenStart();
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse 'with' subject expression");
    recordPauseLocation(context.breakpointLocation(expr));
    JSTextPosition end = lastTokenEndPosition();
    int endLine = tokenLine();
    handleProductionOrFail(CLOSEPAREN, ")", "start", "subject of a 'with' statement");

    AutoPopScopeRef withScope(this, pushScope());
    withScope->setTaintedByWithScope();
    withScope->preventAllVariableDeclarations();

    const Identifier* unused = nullptr;
    TreeStatement statement = parseStatement(context, unused);
    failIfFalse(statement, "A 'with' statement must have a body");
    
    TreeStatement result = context.createWithStatement(location, expr, statement, start, end, startLine, endLine);
    popScope(withScope, TreeBuilder::NeedsFreeVariableInfo);
    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseSwitchStatement(TreeBuilder& context)
{
    ASSERT(match(SWITCH));
    JSTokenLocation location(tokenLocation());
    int startLine = tokenLine();
    next();
    handleProductionOrFail(OPENPAREN, "(", "start", "subject of a 'switch'");
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse switch subject expression");
    recordPauseLocation(context.breakpointLocation(expr));
    int endLine = tokenLine();
    
    handleProductionOrFail(CLOSEPAREN, ")", "end", "subject of a 'switch'");
    handleProductionOrFail(OPENBRACE, "{", "start", "body of a 'switch'");
    AutoPopScopeRef lexicalScope(this, pushScope());
    lexicalScope->setIsLexicalScope();
    lexicalScope->preventVarDeclarations();
    startSwitch();
    TreeClauseList firstClauses = parseSwitchClauses(context);
    propagateError();
    
    TreeClause defaultClause = parseSwitchDefaultClause(context);
    propagateError();
    
    TreeClauseList secondClauses = parseSwitchClauses(context);
    propagateError();
    endSwitch();
    handleProductionOrFail(CLOSEBRACE, "}", "end", "body of a 'switch'");
    
    auto [lexicalEnvironment, functionDeclarations] = popScope(lexicalScope, TreeBuilder::NeedsFreeVariableInfo);
    return context.createSwitchStatement(location, expr, firstClauses, defaultClause, secondClauses, startLine, endLine, WTFMove(lexicalEnvironment), WTFMove(functionDeclarations));
}

template <typename LexerType>
template <class TreeBuilder> TreeClauseList Parser<LexerType>::parseSwitchClauses(TreeBuilder& context)
{
    if (!match(CASE))
        return 0;
    unsigned startOffset = tokenStart();
    next();
    TreeExpression condition = parseExpression(context);
    failIfFalse(condition, "Cannot parse switch clause");
    consumeOrFail(COLON, "Expected a ':' after switch clause expression");
    TreeSourceElements statements = parseSourceElements(context, DontCheckForStrictMode);
    failIfFalse(statements, "Cannot parse the body of a switch clause");
    TreeClause clause = context.createClause(condition, statements);
    context.setStartOffset(clause, startOffset);
    TreeClauseList clauseList = context.createClauseList(clause);
    TreeClauseList tail = clauseList;
    
    while (match(CASE)) {
        startOffset = tokenStart();
        next();
        TreeExpression condition = parseExpression(context);
        failIfFalse(condition, "Cannot parse switch case expression");
        consumeOrFail(COLON, "Expected a ':' after switch clause expression");
        TreeSourceElements statements = parseSourceElements(context, DontCheckForStrictMode);
        failIfFalse(statements, "Cannot parse the body of a switch clause");
        clause = context.createClause(condition, statements);
        context.setStartOffset(clause, startOffset);
        tail = context.createClauseList(tail, clause);
    }
    return clauseList;
}

template <typename LexerType>
template <class TreeBuilder> TreeClause Parser<LexerType>::parseSwitchDefaultClause(TreeBuilder& context)
{
    if (!match(DEFAULT))
        return 0;
    unsigned startOffset = tokenStart();
    next();
    consumeOrFail(COLON, "Expected a ':' after switch default clause");
    TreeSourceElements statements = parseSourceElements(context, DontCheckForStrictMode);
    failIfFalse(statements, "Cannot parse the body of a switch default clause");
    TreeClause result = context.createClause(0, statements);
    context.setStartOffset(result, startOffset);
    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseTryStatement(TreeBuilder& context)
{
    ASSERT(match(TRY));
    JSTokenLocation location(tokenLocation());
    TreeStatement tryBlock = 0;
    TreeDestructuringPattern catchPattern = 0;
    TreeStatement catchBlock = 0;
    TreeStatement finallyBlock = 0;
    int firstLine = tokenLine();
    next();
    matchOrFail(OPENBRACE, "Expected a block statement as body of a try statement");
    
    int returnStatementCountBeforeTryBlock = m_parserState.returnStatementCount;
    tryBlock = parseBlockStatement(context);
    failIfFalse(tryBlock, "Cannot parse the body of try block");
    bool tryBlockContainsReturn = m_parserState.returnStatementCount != returnStatementCountBeforeTryBlock;
    int lastLine = m_lastTokenEndPosition.line;
    VariableEnvironment catchEnvironment; 
    DeclarationStacks::FunctionStack functionStack;
    if (consume(CATCH)) {
        if (match(OPENBRACE)) {
            catchBlock = parseBlockStatement(context);
            failIfFalse(catchBlock, "Unable to parse 'catch' block");
        } else {
            handleProductionOrFail(OPENPAREN, "(", "start", "'catch' target");
            DepthManager statementDepth(&m_statementDepth);
            semanticFailIfTrue(currentScope()->isStaticBlock() && match(AWAIT), "Cannot use 'await' as identifier within static block");
            m_statementDepth++;
            AutoPopScopeRef catchScope(this, pushScope());
            catchScope->setIsLexicalScope();
            catchScope->preventVarDeclarations();
            const Identifier* ident = nullptr;
            if (matchSpecIdentifier()) {
                catchScope->setIsSimpleCatchParameterScope();
                ident = m_token.m_data.ident;
                catchPattern = context.createBindingLocation(m_token.m_location, *ident, m_token.m_startPosition, m_token.m_endPosition, AssignmentContext::DeclarationStatement);
                next();
                failIfTrueIfStrict(catchScope->declareLexicalVariable(ident, false) & DeclarationResult::InvalidStrictMode, "Cannot declare a catch variable named '", ident->impl(), "' in strict mode");
            } else {
                catchPattern = parseDestructuringPattern(context, DestructuringKind::DestructureToCatchParameters, ExportType::NotExported);
                failIfFalse(catchPattern, "Cannot parse this destructuring pattern");
            }
            handleProductionOrFail(CLOSEPAREN, ")", "end", "'catch' target");
            matchOrFail(OPENBRACE, "Expected exception handler to be a block statement");
            catchBlock = parseBlockStatement(context, BlockType::CatchBlock);
            failIfFalse(catchBlock, "Unable to parse 'catch' block");
            std::tie(catchEnvironment, functionStack) = popScope(catchScope, TreeBuilder::NeedsFreeVariableInfo);
            ASSERT(functionStack.isEmpty());
            RELEASE_ASSERT(!ident || (catchEnvironment.size() == 1 && catchEnvironment.contains(ident->impl())));
        }
    }
    
    if (consume(FINALLY)) {
        matchOrFail(OPENBRACE, "Expected block statement for finally body");
        finallyBlock = parseBlockStatement(context);
        failIfFalse(finallyBlock, "Cannot parse finally body");
    }
    failIfFalse(catchBlock || finallyBlock, "Try statements must have at least a catch or finally block");

    if (tryBlockContainsReturn && !finallyBlock && currentFunctionScope()->constructorKind() == ConstructorKind::Extends) {
        // Empty `finally` statement is necessary to prevent BytecodeGenerator::emitReturn() from being
        // called inside the `try` block, which would otherwise result in errors thrown at steps 10-12
        // of https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
        // being caught by the `catch` block.
        finallyBlock = context.createEmptyStatement(location);
    }

    return context.createTryStatement(location, tryBlock, catchPattern, catchBlock, finallyBlock, firstLine, lastLine, WTFMove(catchEnvironment));
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseDebuggerStatement(TreeBuilder& context)
{
    ASSERT(match(DEBUGGER));
    JSTokenLocation location(tokenLocation());
    int startLine = tokenLine();
    int endLine = startLine;
    next();
    if (match(SEMICOLON))
        startLine = tokenLine();
    failIfFalse(autoSemiColon(), "Debugger keyword must be followed by a ';'");
    return context.createDebugger(location, startLine, endLine);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseBlockStatement(TreeBuilder& context, BlockType type)
{
    ASSERT(match(OPENBRACE));

    // We should treat the first block statement of the function (the body of the function) as the lexical 
    // scope of the function itself, and not the lexical scope of a 'block' statement within the function.
    AutoCleanupLexicalScope lexicalScope;
    bool shouldPushLexicalScope = m_statementDepth > 0 || type == BlockType::StaticBlock;
    if (shouldPushLexicalScope) {
        ScopeRef newScope = pushScope();
        newScope->setIsLexicalScope();
        switch (type) {
        case BlockType::CatchBlock:
            newScope->setIsCatchBlockScope();
            newScope->preventVarDeclarations();
            break;
        case BlockType::StaticBlock:
            newScope->setSourceParseMode(SourceParseMode::ClassStaticBlockMode);
            newScope->setExpectedSuperBinding(SuperBinding::Needed);
            break;
        case BlockType::Normal:
            newScope->preventVarDeclarations();
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        lexicalScope.setIsValid(newScope, this);
    }
    JSTokenLocation location(tokenLocation());
    int startOffset = m_token.m_data.offset;
    int start = tokenLine();
    VariableEnvironment lexicalEnvironment;
    DeclarationStacks::FunctionStack functionStack;
    next();
    if (match(CLOSEBRACE)) {
        int endOffset = m_token.m_data.offset;
        next();
        if (shouldPushLexicalScope)
            std::tie(lexicalEnvironment, functionStack) = popScope(lexicalScope, TreeBuilder::NeedsFreeVariableInfo);
        TreeStatement result = context.createBlockStatement(location, 0, start, m_lastTokenEndPosition.line, WTFMove(lexicalEnvironment), WTFMove(functionStack));
        context.setStartOffset(result, startOffset);
        context.setEndOffset(result, endOffset);
        return result;
    }
    TreeSourceElements subtree = parseSourceElements(context, DontCheckForStrictMode);
    failIfFalse(subtree, "Cannot parse the body of the block statement");
    matchOrFail(CLOSEBRACE, "Expected a closing '}' at the end of a block statement");
    int endOffset = m_token.m_data.offset;
    next();
    if (shouldPushLexicalScope)
        std::tie(lexicalEnvironment, functionStack) = popScope(lexicalScope, TreeBuilder::NeedsFreeVariableInfo);
    TreeStatement result = context.createBlockStatement(location, subtree, start, m_lastTokenEndPosition.line, WTFMove(lexicalEnvironment), WTFMove(functionStack));
    context.setStartOffset(result, startOffset);
    context.setEndOffset(result, endOffset);
    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseStatement(TreeBuilder& context, const Identifier*& directive, unsigned* directiveLiteralLength)
{
    DepthManager statementDepth(&m_statementDepth);
    m_statementDepth++;
    int nonTrivialExpressionCount = 0;
    failIfStackOverflow();
    TreeStatement result = 0;
    bool shouldSetEndOffset = true;
    bool shouldSetPauseLocation = false;
    bool parentAllowsFunctionDeclarationAsStatement = m_immediateParentAllowsFunctionDeclarationInStatement;
    m_immediateParentAllowsFunctionDeclarationInStatement = false;

    switch (m_token.m_type) {
    case OPENBRACE:
        result = parseBlockStatement(context);
        shouldSetEndOffset = false;
        break;
    case VAR:
        result = parseVariableDeclaration(context, DeclarationType::VarDeclaration);
        shouldSetPauseLocation = true;
        break;
    case FUNCTION: {
        result = parseFunctionDeclarationStatement(context, parentAllowsFunctionDeclarationAsStatement);
        break;
    }
    case SEMICOLON: {
        JSTokenLocation location(tokenLocation());
        next();
        result = context.createEmptyStatement(location);
        shouldSetPauseLocation = true;
        break;
    }
    case IF:
        result = parseIfStatement(context);
        break;
    case DO:
        result = parseDoWhileStatement(context);
        break;
    case WHILE:
        result = parseWhileStatement(context);
        break;
    case FOR:
        result = parseForStatement(context);
        break;
    case CONTINUE:
        result = parseContinueStatement(context);
        shouldSetPauseLocation = true;
        break;
    case BREAK:
        result = parseBreakStatement(context);
        shouldSetPauseLocation = true;
        break;
    case RETURN:
        result = parseReturnStatement(context);
        shouldSetPauseLocation = true;
        break;
    case WITH:
        result = parseWithStatement(context);
        break;
    case SWITCH:
        result = parseSwitchStatement(context);
        break;
    case THROW:
        result = parseThrowStatement(context);
        shouldSetPauseLocation = true;
        break;
    case TRY:
        result = parseTryStatement(context);
        break;
    case DEBUGGER:
        result = parseDebuggerStatement(context);
        shouldSetPauseLocation = true;
        break;
    case EOFTOK:
    case CASE:
    case CLOSEBRACE:
    case DEFAULT:
        // These tokens imply the end of a set of source elements
        return 0;
    case ESCAPED_KEYWORD:
        if (!matchAllowedEscapedContextualKeyword()) [[unlikely]]
            failDueToUnexpectedToken();
        [[fallthrough]];
    case LET:
    case IDENT:
    case AWAIT:
    case YIELD: {
        bool allowFunctionDeclarationAsStatement = false;
        result = parseExpressionOrLabelStatement(context, allowFunctionDeclarationAsStatement);
        shouldSetPauseLocation = !context.shouldSkipPauseLocation(result);
        break;
    }
    case STRING:
        directive = m_token.m_data.ident;
        if (directiveLiteralLength)
            *directiveLiteralLength = m_token.m_location.endOffset - m_token.m_location.startOffset;
        nonTrivialExpressionCount = m_parserState.nonTrivialExpressionCount;
        [[fallthrough]];
    default:
        TreeStatement exprStatement = parseExpressionStatement(context);
        if (directive && nonTrivialExpressionCount != m_parserState.nonTrivialExpressionCount)
            directive = nullptr;
        result = exprStatement;
        shouldSetPauseLocation = true;
        break;
    }

    if (result) {
        if (shouldSetEndOffset)
            context.setEndOffset(result, m_lastTokenEndPosition.offset);
        if (shouldSetPauseLocation)
            recordPauseLocation(context.breakpointLocation(result));
    }

    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseFunctionDeclarationStatement(TreeBuilder& context, bool parentAllowsFunctionDeclarationAsStatement)
{
    semanticFailIfTrue(strictMode(), "Function declarations are only allowed inside blocks or switch statements in strict mode");
    failIfFalse(parentAllowsFunctionDeclarationAsStatement, "Function declarations are only allowed inside block statements or at the top level of a program");

    // Any function declaration that isn't in a block is a syntax error unless it's
    // in an if/else statement. If it's in an if/else statement, we will magically
    // treat it as if the if/else statement is inside a block statement.
    // to the very top like a "var". For example:
    // function a() {
    //     if (cond) function foo() { }
    // }
    // will be rewritten as:
    // function a() {
    //     if (cond) { function foo() { } }
    // }
    AutoPopScopeRef blockScope(this, pushScope());
    blockScope->setIsLexicalScope();
    blockScope->preventVarDeclarations();
    JSTokenLocation location(tokenLocation());
    int start = tokenLine();

    TreeStatement function = parseFunctionDeclaration(context, FunctionDeclarationType::Statement);
    propagateError();
    failIfFalse(function, "Expected valid function statement after 'function' keyword");
    TreeSourceElements sourceElements = context.createSourceElements();
    context.appendStatement(sourceElements, function);
    auto [lexicalEnvironment, functionDeclarations] = popScope(blockScope, TreeBuilder::NeedsFreeVariableInfo);
    return context.createBlockStatement(location, sourceElements, start, m_lastTokenEndPosition.line, WTFMove(lexicalEnvironment), WTFMove(functionDeclarations));
}

template <typename LexerType>
template <class TreeBuilder> bool Parser<LexerType>::parseFormalParameters(TreeBuilder& context, TreeFormalParameterList list, bool isArrowFunction, bool isMethod, unsigned& parameterCount)
{
#define failIfDuplicateIfViolation() \
    if (duplicateParameter) {\
        semanticFailIfTrue(hasDefaultParameterValues, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in function with default parameter values");\
        semanticFailIfTrue(hasDestructuringPattern, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in function with destructuring parameters");\
        semanticFailIfTrue(isRestParameter, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in function with a rest parameter");\
        semanticFailIfTrue(isArrowFunction, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in an arrow function");\
        semanticFailIfTrue(isMethod, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in a method");\
    }

    bool hasDefaultParameterValues = false;
    bool hasDestructuringPattern = false;
    bool isRestParameter = false;
    const Identifier* duplicateParameter = nullptr;
    unsigned restParameterStart = 0;
    do {
        TreeDestructuringPattern parameter = 0;
        TreeExpression defaultValue = 0;

        if (match(CLOSEPAREN)) [[unlikely]]
            break;
        
        if (consume(DOTDOTDOT)) {
            semanticFailIfTrue(isDisallowedIdentifierAwait(m_token), "Cannot use 'await' as a parameter name in an async function");
            TreeDestructuringPattern destructuringPattern = parseDestructuringPattern(context, DestructuringKind::DestructureToParameters, ExportType::NotExported, &duplicateParameter, &hasDestructuringPattern);
            propagateError();
            parameter = context.createRestParameter(destructuringPattern, restParameterStart);
            failIfTrue(match(COMMA), "Rest parameter should be the last parameter in a function declaration"); // Let's have a good error message for this common case.
            isRestParameter = true;
        } else
            parameter = parseDestructuringPattern(context, DestructuringKind::DestructureToParameters, ExportType::NotExported, &duplicateParameter, &hasDestructuringPattern);
        failIfFalse(parameter, "Cannot parse parameter pattern");
        if (!isRestParameter) {
            defaultValue = parseDefaultValueForDestructuringPattern(context);
            if (defaultValue)
                hasDefaultParameterValues = true;
        }
        propagateError();
        failIfDuplicateIfViolation();
        if (isRestParameter || defaultValue || hasDestructuringPattern)
            currentScope()->setHasNonSimpleParameterList();
        context.appendParameter(list, parameter, defaultValue);
        if (!isRestParameter) {
            restParameterStart++;
            if (!hasDefaultParameterValues)
                parameterCount++;
        }
    } while (!isRestParameter && consume(COMMA));

    return true;
#undef failIfDuplicateIfViolation
}

static ALWAYS_INLINE SuperBinding adjustSuperBindingForBaseConstructor(ConstructorKind constructorKind, SuperBinding expectedSuperBinding, SourceParseMode parseMode, bool scopeNeedsSuperBinding, bool currentScopeUsesEval, InnerArrowFunctionCodeFeatures innerArrowFunctionFeatures)
{
    if (expectedSuperBinding == SuperBinding::NotNeeded)
        return SuperBinding::NotNeeded;

    if (constructorKind == ConstructorKind::None) {
        if (SourceParseModeSet(
                SourceParseMode::AsyncGeneratorWrapperMethodMode,
                SourceParseMode::GeneratorWrapperMethodMode,
                SourceParseMode::AsyncMethodMode
                ).contains(parseMode))
            return SuperBinding::Needed;
    }

    if (constructorKind == ConstructorKind::None || constructorKind == ConstructorKind::Base) {
        bool isSuperUsedInInnerArrowFunction = innerArrowFunctionFeatures & SuperPropertyInnerArrowFunctionFeature;
        return (scopeNeedsSuperBinding || isSuperUsedInInnerArrowFunction || currentScopeUsesEval) ? SuperBinding::Needed : SuperBinding::NotNeeded;
    }

    return SuperBinding::Needed;
}

static ALWAYS_INLINE SuperBinding adjustSuperBindingForBaseConstructor(ConstructorKind constructorKind, SuperBinding expectedSuperBinding, SourceParseMode parseMode, ScopeRef functionScope)
{
    return adjustSuperBindingForBaseConstructor(constructorKind, expectedSuperBinding, parseMode, functionScope->needsSuperBinding(), functionScope->usesEval(), functionScope->innerArrowFunctionFeatures());
}

template <typename LexerType>
template <class TreeBuilder> TreeFunctionBody Parser<LexerType>::parseFunctionBody(
    TreeBuilder& context, SyntaxChecker& syntaxChecker, const JSTokenLocation& startLocation, int startColumn, unsigned functionStart, int functionNameStart, int parametersStart,
    ConstructorKind constructorKind, SuperBinding superBinding, FunctionBodyType bodyType, unsigned parameterCount)
{
    SetForScope overrideParsingClassFieldInitializer(m_parserState.isParsingClassFieldInitializer, bodyType == StandardFunctionBodyBlock ? false : m_parserState.isParsingClassFieldInitializer);
    SetForScope maybeUnmaskAsync(m_parserState.classFieldInitMasksAsync, isAsyncFunctionParseMode(m_parseMode) ? false : m_parserState.classFieldInitMasksAsync);
    bool isArrowFunctionBodyExpression = bodyType == ArrowFunctionBodyExpression;
    if (!isArrowFunctionBodyExpression) {
        next();
        if (match(CLOSEBRACE)) {
            unsigned endColumn = tokenColumn();
            SuperBinding functionSuperBinding = adjustSuperBindingForBaseConstructor(constructorKind, superBinding, sourceParseMode(), currentScope());
            return context.createFunctionMetadata(startLocation, tokenLocation(), startColumn, endColumn, functionStart, functionNameStart, parametersStart, implementationVisibility(), lexicallyScopedFeatures(), constructorKind, functionSuperBinding, parameterCount, sourceParseMode(), isArrowFunctionBodyExpression);
        }
    }

    DepthManager statementDepth(&m_statementDepth);
    m_statementDepth = 0;
    if (bodyType == ArrowFunctionBodyExpression) {
        if (m_debuggerParseData)
            failIfFalse(parseArrowFunctionSingleExpressionBodySourceElements(context), "Cannot parse body of this arrow function");
        else
            failIfFalse(parseArrowFunctionSingleExpressionBodySourceElements(syntaxChecker), "Cannot parse body of this arrow function");
    } else {
        if (m_debuggerParseData)
            failIfFalse(parseSourceElements(context, CheckForStrictMode), bodyType == StandardFunctionBodyBlock ? "Cannot parse body of this function" : "Cannot parse body of this arrow function");
        else
            failIfFalse(parseSourceElements(syntaxChecker, CheckForStrictMode), bodyType == StandardFunctionBodyBlock ? "Cannot parse body of this function" : "Cannot parse body of this arrow function");
    }
    unsigned endColumn = tokenColumn();
    SuperBinding functionSuperBinding = adjustSuperBindingForBaseConstructor(constructorKind, superBinding, sourceParseMode(), currentScope());
    return context.createFunctionMetadata(startLocation, tokenLocation(), startColumn, endColumn, functionStart, functionNameStart, parametersStart, implementationVisibility(), lexicallyScopedFeatures(), constructorKind, functionSuperBinding, parameterCount, sourceParseMode(), isArrowFunctionBodyExpression);
}

static const char* stringArticleForFunctionMode(SourceParseMode mode)
{
    switch (mode) {
    case SourceParseMode::GetterMode:
    case SourceParseMode::SetterMode:
    case SourceParseMode::NormalFunctionMode:
    case SourceParseMode::MethodMode:
    case SourceParseMode::GeneratorBodyMode:
    case SourceParseMode::GeneratorWrapperFunctionMode:
    case SourceParseMode::GeneratorWrapperMethodMode:
        return "a ";
    case SourceParseMode::ArrowFunctionMode:
    case SourceParseMode::AsyncFunctionMode:
    case SourceParseMode::AsyncFunctionBodyMode:
    case SourceParseMode::AsyncMethodMode:
    case SourceParseMode::AsyncArrowFunctionBodyMode:
    case SourceParseMode::AsyncArrowFunctionMode:
    case SourceParseMode::AsyncGeneratorWrapperFunctionMode:
    case SourceParseMode::AsyncGeneratorBodyMode:
    case SourceParseMode::AsyncGeneratorWrapperMethodMode:
        return "an ";
    case SourceParseMode::ProgramMode:
    case SourceParseMode::ModuleAnalyzeMode:
    case SourceParseMode::ModuleEvaluateMode:
    case SourceParseMode::ClassFieldInitializerMode:
    case SourceParseMode::ClassStaticBlockMode:
        RELEASE_ASSERT_NOT_REACHED();
        return "";
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}
    
static const char* stringForFunctionMode(SourceParseMode mode)
{
    switch (mode) {
    case SourceParseMode::GetterMode:
        return "getter";
    case SourceParseMode::SetterMode:
        return "setter";
    case SourceParseMode::NormalFunctionMode:
        return "function";
    case SourceParseMode::MethodMode:
        return "method";
    case SourceParseMode::GeneratorWrapperFunctionMode:
    case SourceParseMode::GeneratorBodyMode:
        return "generator function";
    case SourceParseMode::GeneratorWrapperMethodMode:
        return "generator method";
    case SourceParseMode::ArrowFunctionMode:
        return "arrow function";
    case SourceParseMode::AsyncFunctionMode:
    case SourceParseMode::AsyncFunctionBodyMode:
        return "async function";
    case SourceParseMode::AsyncMethodMode:
        return "async method";
    case SourceParseMode::AsyncArrowFunctionBodyMode:
    case SourceParseMode::AsyncArrowFunctionMode:
        return "async arrow function";
    case SourceParseMode::AsyncGeneratorWrapperFunctionMode:
    case SourceParseMode::AsyncGeneratorBodyMode:
        return "async generator function";
    case SourceParseMode::AsyncGeneratorWrapperMethodMode:
        return "async generator method";
    case SourceParseMode::ProgramMode:
    case SourceParseMode::ModuleAnalyzeMode:
    case SourceParseMode::ModuleEvaluateMode:
    case SourceParseMode::ClassFieldInitializerMode:
    case SourceParseMode::ClassStaticBlockMode:
        RELEASE_ASSERT_NOT_REACHED();
        return "";
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

template <typename LexerType> template <class TreeBuilder, class FunctionInfoType> typename TreeBuilder::FormalParameterList Parser<LexerType>::parseFunctionParameters(TreeBuilder& context, FunctionInfoType& functionInfo)
{
    auto mode = sourceParseMode();
    RELEASE_ASSERT(!(SourceParseModeSet(SourceParseMode::ProgramMode, SourceParseMode::ModuleAnalyzeMode, SourceParseMode::ModuleEvaluateMode).contains(mode)));
    TreeFormalParameterList parameterList = context.createFormalParameterList();
    if (mode == SourceParseMode::ClassStaticBlockMode)
        return parameterList;
    SetForScope functionParsePhasePoisoner(m_parserState.functionParsePhase, FunctionParsePhase::Parameters);

    if ((SourceParseModeSet(SourceParseMode::ArrowFunctionMode, SourceParseMode::AsyncArrowFunctionMode).contains(mode))) [[unlikely]] {
        if (!matchSpecIdentifier() && !match(OPENPAREN)) [[unlikely]] {
            semanticFailureDueToKeyword(stringForFunctionMode(mode), " name");
            failWithMessage("Expected an arrow function input parameter");
        }

        if (consume(OPENPAREN)) {
            if (match(CLOSEPAREN))
                functionInfo.parameterCount = 0;
            else {
                bool isArrowFunction = true;
                bool isMethod = false;
                failIfFalse(parseFormalParameters(context, parameterList, isArrowFunction, isMethod, functionInfo.parameterCount), "Cannot parse parameters for this ", stringForFunctionMode(mode));
            }

            consumeOrFail(CLOSEPAREN, "Expected a ')' or a ',' after a parameter declaration");
        } else {
            functionInfo.parameterCount = 1;
            auto parameter = parseDestructuringPattern(context, DestructuringKind::DestructureToParameters, ExportType::NotExported);
            failIfFalse(parameter, "Cannot parse parameter pattern");
            context.appendParameter(parameterList, parameter, 0);
        }

        return parameterList;
    }

    if (!consume(OPENPAREN)) [[unlikely]] {
        semanticFailureDueToKeyword(stringForFunctionMode(mode), " name");
        failWithMessage("Expected an opening '(' before a ", stringForFunctionMode(mode), "'s parameter list");
    }

    if (mode == SourceParseMode::GetterMode) {
        consumeOrFail(CLOSEPAREN, "getter functions must have no parameters");
        functionInfo.parameterCount = 0;
    } else if (mode == SourceParseMode::SetterMode) {
        failIfTrue(match(CLOSEPAREN), "setter functions must have one parameter");
        const Identifier* duplicateParameter = nullptr;
        bool hasDestructuringPattern = false;
        auto parameter = parseDestructuringPattern(context, DestructuringKind::DestructureToParameters, ExportType::NotExported, &duplicateParameter, &hasDestructuringPattern);
        failIfFalse(parameter, "setter functions must have one parameter");
        auto defaultValue = parseDefaultValueForDestructuringPattern(context);
        propagateError();
        if (defaultValue || hasDestructuringPattern) {
            semanticFailIfTrue(duplicateParameter, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in function with non-simple parameter list");
            currentScope()->setHasNonSimpleParameterList();
        }
        context.appendParameter(parameterList, parameter, defaultValue);
        functionInfo.parameterCount = defaultValue ? 0 : 1;
        failIfTrue(match(COMMA), "setter functions must have one parameter");
        consumeOrFail(CLOSEPAREN, "Expected a ')' after a parameter declaration");
    } else {
        if (match(CLOSEPAREN)) {
            functionInfo.parameterCount = 0;
        } else {
            bool isArrowFunction = false;
            bool isMethod = isMethodParseMode(mode);
            failIfFalse(parseFormalParameters(context, parameterList, isArrowFunction, isMethod, functionInfo.parameterCount), "Cannot parse parameters for this ", stringForFunctionMode(mode));
        }
        consumeOrFail(CLOSEPAREN, "Expected a ')' or a ',' after a parameter declaration");
    }

    return parameterList;
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::FormalParameterList Parser<LexerType>::createGeneratorParameters(TreeBuilder& context, unsigned& parameterCount)
{
    auto parameters = context.createFormalParameterList();

    JSTokenLocation location(tokenLocation());
    JSTextPosition position = tokenStartPosition();

    auto addParameter = [&](const Identifier& name) {
        declareParameter(&name);
        auto binding = context.createBindingLocation(location, name, position, position, AssignmentContext::DeclarationStatement);
        context.appendParameter(parameters, binding, 0);
        ++parameterCount;
    };

    // @generator
    addParameter(m_vm.propertyNames->generatorPrivateName);
    // @generatorState
    addParameter(m_vm.propertyNames->generatorStatePrivateName);
    // @generatorValue
    addParameter(m_vm.propertyNames->generatorValuePrivateName);
    // @generatorResumeMode
    addParameter(m_vm.propertyNames->generatorResumeModePrivateName);
    // @generatorFrame
    addParameter(m_vm.propertyNames->generatorFramePrivateName);

    return parameters;
}

template <typename LexerType>
template <class TreeBuilder> bool Parser<LexerType>::parseFunctionInfo(TreeBuilder& context, FunctionNameRequirements requirements, bool nameIsInContainingScope, ConstructorKind constructorKind, SuperBinding expectedSuperBinding, unsigned functionStart, ParserFunctionInfo<TreeBuilder>& functionInfo, FunctionDefinitionType functionDefinitionType, std::optional<int> functionConstructorParametersEndPosition)
{
    auto mode = sourceParseMode();
    RELEASE_ASSERT(isFunctionParseMode(mode));

    ScopeRef parentScope = currentScope();

    bool functionNameIsAwait = isPossiblyEscapedAwait(m_token);
    const char* isDisallowedAwaitFunctionNameReason = functionNameIsAwait && !canUseIdentifierAwait() ? disallowedIdentifierAwaitReason() : nullptr;

    AutoPopScopeRef functionScope(this, pushScope());

    functionScope->setSourceParseMode(mode);
    resetImplementationVisibilityIfNeeded();

    functionScope->setExpectedSuperBinding(expectedSuperBinding);
    functionScope->setConstructorKind(constructorKind);

    SetForScope functionParsePhasePoisoner(m_parserState.functionParsePhase, FunctionParsePhase::Body);
    int functionNameStart = m_token.m_location.startOffset;
    const Identifier* lastFunctionName = m_parserState.lastFunctionName;
    m_parserState.lastFunctionName = nullptr;
    int parametersStart = -1;
    JSTokenLocation startLocation;
    int startColumn = -1;
    FunctionBodyType functionBodyType;

    auto tryLoadCachedFunction = [&] () -> bool {
        if (!Options::useSourceProviderCache()) [[unlikely]]
            return false;

        if (m_debuggerParseData) [[unlikely]]
            return false;

        ASSERT(parametersStart != -1);
        ASSERT(startColumn != -1);

        // If we know about this function already, we can use the cached info and skip the parser to the end of the function.
        if (const SourceProviderCacheItem* cachedInfo = TreeBuilder::CanUseFunctionCache ? findCachedFunctionInfo(parametersStart) : nullptr) {
            // If we're in a strict context, the cached function info must say it was strict too.
            ASSERT(!strictMode() || (cachedInfo->lexicallyScopedFeatures() & StrictModeLexicallyScopedFeature));
            JSTokenLocation endLocation;

            ConstructorKind constructorKind = static_cast<ConstructorKind>(cachedInfo->constructorKind);
            SuperBinding expectedSuperBinding = static_cast<SuperBinding>(cachedInfo->expectedSuperBinding);

            endLocation.line = cachedInfo->lastTokenLine;
            endLocation.startOffset = cachedInfo->lastTokenStartOffset;
            endLocation.lineStartOffset = cachedInfo->lastTokenLineStartOffset;
            ASSERT(endLocation.startOffset >= endLocation.lineStartOffset);

            bool endColumnIsOnStartLine = endLocation.line == functionInfo.startLine;
            unsigned currentLineStartOffset = m_lexer->currentLineStartOffset();
            unsigned bodyEndColumn = endColumnIsOnStartLine ? endLocation.startOffset - currentLineStartOffset : endLocation.startOffset - endLocation.lineStartOffset;

            ASSERT(endLocation.startOffset >= endLocation.lineStartOffset);
            
            FunctionBodyType functionBodyType;
            if (SourceParseModeSet(SourceParseMode::ArrowFunctionMode, SourceParseMode::AsyncArrowFunctionMode).contains(mode)) [[unlikely]]
                functionBodyType = cachedInfo->isBodyArrowExpression ?  ArrowFunctionBodyExpression : ArrowFunctionBodyBlock;
            else
                functionBodyType = StandardFunctionBodyBlock;

            SuperBinding functionSuperBinding = adjustSuperBindingForBaseConstructor(constructorKind, expectedSuperBinding, mode, cachedInfo->needsSuperBinding, cachedInfo->usesEval, cachedInfo->innerArrowFunctionFeatures);

            // Grab this from the current `Scope` instead of saving it to `SourceProviderCacheItem`
            // since it's trivial to compute each time.
            auto implementationVisibility = this->implementationVisibility();

            functionInfo.body = context.createFunctionMetadata(
                startLocation, endLocation, startColumn, bodyEndColumn, functionStart,
                functionNameStart, parametersStart, implementationVisibility,
                cachedInfo->lexicallyScopedFeatures(), constructorKind, functionSuperBinding,
                cachedInfo->parameterCount,
                mode, functionBodyType == ArrowFunctionBodyExpression);
            functionInfo.endOffset = cachedInfo->endFunctionOffset;
            functionInfo.parameterCount = cachedInfo->parameterCount;

            functionScope->restoreFromSourceProviderCache(cachedInfo);
            popScope(functionScope, TreeBuilder::NeedsFreeVariableInfo);
            
            m_token = cachedInfo->endFunctionToken();

            if (endColumnIsOnStartLine)
                m_token.m_location.lineStartOffset = currentLineStartOffset;

            m_lexer->setOffset(m_token.m_location.endOffset, m_token.m_location.lineStartOffset);
            m_lexer->setLineNumber(m_token.m_location.line);

            switch (functionBodyType) {
            case ArrowFunctionBodyExpression:
                next();
                context.setEndOffset(functionInfo.body, m_lexer->currentOffset());
                break;
            case ArrowFunctionBodyBlock:
            case StandardFunctionBodyBlock:
                context.setEndOffset(functionInfo.body, m_lexer->currentOffset());
                next();
                break;
            }
            functionInfo.endLine = m_lastTokenEndPosition.line;
            return true;
        }

        return false;
    };

    SyntaxChecker syntaxChecker(const_cast<VM&>(m_vm), m_lexer.get());

    ParserState oldState;
    if ((SourceParseModeSet(SourceParseMode::ArrowFunctionMode, SourceParseMode::AsyncArrowFunctionMode).contains(mode))) [[unlikely]] {
        startLocation = tokenLocation();
        functionInfo.startLine = tokenLine();
        startColumn = tokenColumn();

        parametersStart = m_token.m_location.startOffset;
        functionInfo.startOffset = parametersStart;
        functionInfo.parametersStartColumn = startColumn;

        if (tryLoadCachedFunction())
            return true;

        m_parserState.lastFunctionName = lastFunctionName;
        oldState = internalSaveParserState(context);
        {
            // Parse formal parameters with [+Yield] parameterization, in order to ban YieldExpressions
            // in ArrowFormalParameters, per ES6 #sec-arrow-function-definitions-static-semantics-early-errors.
            Scope::MaybeParseAsGeneratorFunctionForScope parseAsGeneratorFunction(functionScope, parentScope->isGeneratorFunction());
            SetForScope overrideAllowAwait(m_parserState.allowAwait, !parentScope->isAsyncFunction() && !isAsyncFunctionParseMode(mode));
            parseFunctionParameters(syntaxChecker, functionInfo);
            propagateError();
        }

        matchOrFail(ARROWFUNCTION, "Expected a '=>' after arrow function parameter declaration");

        if (m_lexer->hasLineTerminatorBeforeToken()) [[unlikely]]
            failDueToUnexpectedToken();

        ASSERT(constructorKind == ConstructorKind::None);

        // Check if arrow body start with {. If it true it mean that arrow function is Fat arrow function
        // and we need use common approach to parse function body
        next();
        functionBodyType = match(OPENBRACE) ? ArrowFunctionBodyBlock : ArrowFunctionBodyExpression;
    } else {
        // http://ecma-international.org/ecma-262/6.0/#sec-function-definitions
        // FunctionExpression :
        //     function BindingIdentifieropt ( FormalParameters ) { FunctionBody }
        //
        // FunctionDeclaration[Yield, Default] :
        //     function BindingIdentifier[?Yield] ( FormalParameters ) { FunctionBody }
        //     [+Default] function ( FormalParameters ) { FunctionBody }
        //
        // GeneratorDeclaration[Yield, Default] :
        //     function * BindingIdentifier[?Yield] ( FormalParameters[Yield] ) { GeneratorBody }
        //     [+Default] function * ( FormalParameters[Yield] ) { GeneratorBody }
        //
        // GeneratorExpression :
        //     function * BindingIdentifier[Yield]opt ( FormalParameters[Yield] ) { GeneratorBody }
        //
        // The name of FunctionExpression and AsyncFunctionExpression can accept "yield" even in the context of generator.
        bool canUseYield = !strictMode();
        if (!(functionDefinitionType == FunctionDefinitionType::Expression && SourceParseModeSet(SourceParseMode::NormalFunctionMode, SourceParseMode::AsyncFunctionMode).contains(mode)))
            canUseYield &= !parentScope->isGeneratorFunction();

        if (requirements != FunctionNameRequirements::Unnamed) {
            ASSERT_WITH_MESSAGE(!(requirements == FunctionNameRequirements::None && !functionInfo.name), "When specifying FunctionNameRequirements::None, we need to initialize functionInfo.name with the default value in the caller side.");
            if (matchSpecIdentifier(canUseYield, functionNameIsAwait)) {
                functionInfo.name = m_token.m_data.ident;
                m_parserState.lastFunctionName = functionInfo.name;
                if (isDisallowedAwaitFunctionNameReason) [[unlikely]]
                    semanticFailIfTrue(functionDefinitionType == FunctionDefinitionType::Declaration || isAsyncFunctionOrAsyncGeneratorWrapperParseMode(mode), "Cannot declare function named 'await' ", isDisallowedAwaitFunctionNameReason);
                else if (isAsyncFunctionOrAsyncGeneratorWrapperParseMode(mode) && match(AWAIT) && functionDefinitionType == FunctionDefinitionType::Expression) [[unlikely]]
                    semanticFail("Cannot declare ", stringForFunctionMode(mode), " named 'await'");
                else if (isGeneratorOrAsyncGeneratorWrapperParseMode(mode) && match(YIELD) && functionDefinitionType == FunctionDefinitionType::Expression) [[unlikely]]
                    semanticFail("Cannot declare ", stringForFunctionMode(mode), " named 'yield'");
                next();
                if (!nameIsInContainingScope)
                    failIfTrueIfStrict(functionScope->declareCallee(functionInfo.name) & DeclarationResult::InvalidStrictMode, "'", functionInfo.name->impl(), "' is not a valid ", stringForFunctionMode(mode), " name in strict mode");
            } else if (requirements == FunctionNameRequirements::Named) [[unlikely]] {
                if (match(OPENPAREN)) {
                    semanticFailIfTrue(mode == SourceParseMode::NormalFunctionMode, "Function statements must have a name");
                    semanticFailIfTrue(mode == SourceParseMode::AsyncFunctionMode, "Async function statements must have a name");
                }
                semanticFailureDueToKeyword(stringForFunctionMode(mode), " name");
                failDueToUnexpectedToken();
            }
            ASSERT(functionInfo.name);
        }

        startLocation = tokenLocation();
        functionInfo.startLine = tokenLine();
        startColumn = tokenColumn();
        functionInfo.parametersStartColumn = startColumn;

        parametersStart = m_token.m_location.startOffset;
        functionInfo.startOffset = parametersStart;

        if (tryLoadCachedFunction())
            return true;

        m_parserState.lastFunctionName = lastFunctionName;
        oldState = internalSaveParserState(context);
        {
            SetForScope overrideAllowAwait(m_parserState.allowAwait, !isAsyncFunctionParseMode(mode));
            parseFunctionParameters(syntaxChecker, functionInfo);
            propagateError();
        }
        
        matchOrFail(OPENBRACE, "Expected an opening '{' at the start of a ", stringForFunctionMode(mode), " body");

        // If the code is invoked from function constructor, we need to ensure that parameters are only composed by the string offered as parameters.
        if (functionConstructorParametersEndPosition) [[unlikely]]
            semanticFailIfFalse(lastTokenEndPosition().offset == *functionConstructorParametersEndPosition, "Parameters should match arguments offered as parameters in Function constructor");
        
        // BytecodeGenerator emits code to throw TypeError when a class constructor is "call"ed.
        // Set ConstructorKind to None for non-constructor methods of classes.
    
        functionBodyType = StandardFunctionBodyBlock;
    }

    // FIXME: https://bugs.webkit.org/show_bug.cgi?id=156962
    // This loop collects the set of capture candidates that aren't
    // part of the set of this function's declared parameters. We will
    // figure out which parameters are captured for this function when
    // we actually generate code for it. For now, we just propagate to
    // our parent scopes which variables we might have closed over that
    // belong to them. This is necessary for correctness when using
    // the source provider cache because we can't close over a variable
    // that we don't claim to close over. The source provider cache must
    // know this information to properly cache this function.
    // This might work itself out nicer if we declared a different
    // Scope struct for the parameters (because they are indeed implemented
    // as their own scope).
    UniquedStringImplPtrSet nonLocalCapturesFromParameterExpressions;
    functionScope->forEachUsedVariable([&] (UniquedStringImpl* impl) {
        if (!functionScope->hasDeclaredParameter(impl)) {
            nonLocalCapturesFromParameterExpressions.add(impl);
            if (TreeBuilder::NeedsFreeVariableInfo)
                parentScope->addClosedVariableCandidateUnconditionally(impl);
        }
        return IterationStatus::Continue;
    });

    auto performParsingFunctionBody = [&] {
        return parseFunctionBody(context, syntaxChecker, startLocation, startColumn, functionStart, functionNameStart, parametersStart, constructorKind, expectedSuperBinding, functionBodyType, functionInfo.parameterCount);
    };

    if (isGeneratorOrAsyncFunctionWrapperParseMode(mode)) {
        AutoPopScopeRef generatorBodyScope(this, pushScope());
        SourceParseMode innerParseMode = isAsyncFunctionOrAsyncGeneratorWrapperParseMode(mode) ? getAsyncFunctionBodyParseMode(mode) : SourceParseMode::GeneratorBodyMode;

        generatorBodyScope->setSourceParseMode(innerParseMode);
        resetImplementationVisibilityIfNeeded();

        generatorBodyScope->setConstructorKind(ConstructorKind::None);
        generatorBodyScope->setExpectedSuperBinding(expectedSuperBinding);

        // Disallow 'use strict' directives in the implicit inner function if
        // needed.
        if (functionScope->hasNonSimpleParameterList())
            generatorBodyScope->setHasNonSimpleParameterList();

        functionInfo.body = performParsingFunctionBody();

        // When a generator has a "use strict" directive, a generator function wrapping it should be strict mode.
        if  (generatorBodyScope->strictMode())
            functionScope->setStrictMode();

        popScope(generatorBodyScope, TreeBuilder::NeedsFreeVariableInfo);
    } else
        functionInfo.body = performParsingFunctionBody();
    
    restoreParserState(context, oldState);
    failIfFalse(functionInfo.body, "Cannot parse the body of this ", stringForFunctionMode(mode));
    context.setEndOffset(functionInfo.body, m_lexer->currentOffset());
    if (functionScope->strictMode() && requirements != FunctionNameRequirements::Unnamed) {
        ASSERT(functionInfo.name);
        RELEASE_ASSERT(SourceParseModeSet(SourceParseMode::NormalFunctionMode, SourceParseMode::MethodMode, SourceParseMode::ArrowFunctionMode, SourceParseMode::GeneratorBodyMode, SourceParseMode::GeneratorWrapperFunctionMode, SourceParseMode::ClassStaticBlockMode).contains(mode) || isAsyncFunctionOrAsyncGeneratorWrapperParseMode(mode));
        semanticFailIfTrue(m_vm.propertyNames->arguments == *functionInfo.name, "'", functionInfo.name->impl(), "' is not a valid function name in strict mode");
        semanticFailIfTrue(m_vm.propertyNames->eval == *functionInfo.name, "'", functionInfo.name->impl(), "' is not a valid function name in strict mode");
        semanticFailIfTrue(m_vm.propertyNames->yieldKeyword == *functionInfo.name, "'", functionInfo.name->impl(), "' is not a valid function name in strict mode");
    }

    JSTokenLocation location = JSTokenLocation(m_token.m_location);
    functionInfo.endOffset = m_token.m_data.offset;
    
    if (functionBodyType == ArrowFunctionBodyExpression) {
        location = locationBeforeLastToken();
        functionInfo.endOffset = location.endOffset;
    } else {
        recordFunctionEntryLocation(JSTextPosition(startLocation.line, startLocation.startOffset, startLocation.lineStartOffset));
        recordFunctionLeaveLocation(JSTextPosition(location.line, location.startOffset, location.lineStartOffset));
    }

    // Cache the tokenizer state and the function scope the first time the function is parsed.
    // Any future reparsing can then skip the function.
    // For arrow function is 8 = x=>x + 4 symbols;
    // For ordinary function is 16  = function(){} + 4 symbols
    const int minimumSourceLengthToCache = functionBodyType == StandardFunctionBodyBlock ? 16 : 8;
    std::unique_ptr<SourceProviderCacheItem> newInfo;
    int sourceLength = functionInfo.endOffset - functionInfo.startOffset;
    if (TreeBuilder::CanUseFunctionCache && m_functionCache && sourceLength > minimumSourceLengthToCache) {
        SourceProviderCacheItemCreationParameters parameters;
        parameters.endFunctionOffset = functionInfo.endOffset;
        parameters.lastTokenLine = location.line;
        parameters.lastTokenStartOffset = location.startOffset;
        parameters.lastTokenEndOffset = location.endOffset;
        parameters.lastTokenLineStartOffset = location.lineStartOffset;
        parameters.parameterCount = functionInfo.parameterCount;
        parameters.constructorKind = constructorKind;
        parameters.expectedSuperBinding = expectedSuperBinding;
        if (functionBodyType == ArrowFunctionBodyExpression) {
            parameters.isBodyArrowExpression = true;
            parameters.tokenType = m_token.m_type;
        }
        functionScope->fillParametersForSourceProviderCache(parameters, nonLocalCapturesFromParameterExpressions);
        newInfo = SourceProviderCacheItem::create(parameters);
    }

    bool functionScopeWasStrictMode = functionScope->strictMode();
    
    popScope(functionScope, TreeBuilder::NeedsFreeVariableInfo);
    
    if (functionBodyType != ArrowFunctionBodyExpression)
        consumeOrFail(CLOSEBRACE, "Expected a closing '}' after a ", stringForFunctionMode(mode), " body");
    else {
        // We need to lex the last token again because the last token is lexed under the different context because of the following possibilities.
        // 1. which may have different strict mode.
        // 2. which may not build strings for tokens.
        // But (1) is not possible because we do not recognize the string literal in ArrowFunctionBodyExpression as directive and this is correct in terms of the spec (`value => "use strict"`).
        // So we only check TreeBuilder's type here.
        ASSERT_UNUSED(functionScopeWasStrictMode, functionScopeWasStrictMode == currentScope()->strictMode());
        if (!std::is_same<TreeBuilder, SyntaxChecker>::value)
            lexCurrentTokenAgainUnderCurrentContext(context);
    }

    if (newInfo)
        m_functionCache->add(functionInfo.startOffset, WTFMove(newInfo));
    
    functionInfo.endLine = m_lastTokenEndPosition.line;
    return true;
}

static NO_RETURN_DUE_TO_CRASH FunctionMetadataNode* getMetadata(ParserFunctionInfo<SyntaxChecker>&) { RELEASE_ASSERT_NOT_REACHED(); }
static FunctionMetadataNode* getMetadata(ParserFunctionInfo<ASTBuilder>& info) { return info.body; }

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseFunctionDeclaration(TreeBuilder& context, FunctionDeclarationType declarationType, ExportType exportType, DeclarationDefaultContext declarationDefaultContext, std::optional<int> functionConstructorParametersEndPosition)
{
    ASSERT(match(FUNCTION));
    JSTokenLocation location(tokenLocation());
    unsigned functionStart = tokenStart();
    next();
    SourceParseMode parseMode = SourceParseMode::NormalFunctionMode;
    if (match(TIMES)) {
        failIfTrue(declarationType == FunctionDeclarationType::Statement, "Cannot use generator function declaration in single-statement context");
        next();
        parseMode = SourceParseMode::GeneratorWrapperFunctionMode;
    }
    SetForScope innerParseMode(m_parseMode, parseMode);

    ParserFunctionInfo<TreeBuilder> functionInfo;
    FunctionNameRequirements requirements = FunctionNameRequirements::Named;
    if (declarationDefaultContext == DeclarationDefaultContext::ExportDefault) {
        // Under the "export default" context, function declaration does not require the function name.
        //
        //     ExportDeclaration:
        //         ...
        //         export default HoistableDeclaration[~Yield, +Default]
        //         ...
        //
        //     HoistableDeclaration[Yield, Default]:
        //         FunctionDeclaration[?Yield, ?Default]
        //         GeneratorDeclaration[?Yield, ?Default]
        //
        //     FunctionDeclaration[Yield, Default]:
        //         ...
        //         [+Default] function ( FormalParameters[~Yield] ) { FunctionBody[~Yield] }
        //
        //     GeneratorDeclaration[Yield, Default]:
        //         ...
        //         [+Default] function * ( FormalParameters[+Yield] ) { GeneratorBody }
        //
        // In this case, we use "*default*" as this function declaration's name.
        requirements = FunctionNameRequirements::None;
        functionInfo.name = &m_vm.propertyNames->starDefaultPrivateName;
    }

    failIfFalse((parseFunctionInfo(context, requirements, true, ConstructorKind::None, SuperBinding::NotNeeded, functionStart, functionInfo, FunctionDefinitionType::Declaration, functionConstructorParametersEndPosition)), "Cannot parse this function");
    ASSERT(functionInfo.name);

    std::pair<DeclarationResultMask, ScopeRef> functionDeclaration = declareFunction(functionInfo.name);
    DeclarationResultMask declarationResult = functionDeclaration.first;
    failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare a function named '", functionInfo.name->impl(), "' in strict mode");
    semanticFailIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare a function that shadows a let/const/class/function variable '", functionInfo.name->impl(), "'");
    if (exportType == ExportType::Exported) {
        ASSERT_WITH_MESSAGE(declarationDefaultContext != DeclarationDefaultContext::ExportDefault, "Export default case will export the name and binding in the caller.");
        semanticFailIfFalse(exportName(*functionInfo.name), "Cannot export a duplicate function name: '", functionInfo.name->impl(), "'");
        m_moduleScopeData->exportBinding(*functionInfo.name);
    }

    TreeStatement result = context.createFuncDeclStatement(location, functionInfo);
    if (TreeBuilder::CreatesAST) {
        FunctionMetadataNode* metadata = getMetadata(functionInfo);
        functionDeclaration.second->appendFunction(metadata);
        bool isSloppyModeHoistingCandidate = m_statementDepth != 1 && !strictMode() && m_parseMode == SourceParseMode::NormalFunctionMode;
        if (isSloppyModeHoistingCandidate) {
            // Functions declared inside a function inside a nested block scope in sloppy mode are subject to this
            // crazy rule defined inside Annex B.3.2 in the ECMA-262 spec. It basically states that we will create
            // the function as a local block scoped variable, but when we evaluate the block that the function is
            // contained in, we will assign the function to a "var" variable only if declaring such a "var" wouldn't
            // be a syntax error and if there isn't a parameter with the same name. (It would only be a syntax error if
            // there are is a let/class/const with the same name). Note that this mean we only do the "var" hoisting
            // binding if the block evaluates. For example, this means we wont won't perform the binding if it's inside
            // the untaken branch of an if statement.
            functionDeclaration.second->addSloppyModeFunctionHoistingCandidate<Scope::NeedsDuplicateDeclarationCheck::No>(metadata);
        }
    }
    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseAsyncFunctionDeclaration(TreeBuilder& context, unsigned functionStart, ExportType exportType, DeclarationDefaultContext declarationDefaultContext, std::optional<int> functionConstructorParametersEndPosition)
{
    ASSERT(match(FUNCTION));
    JSTokenLocation location(tokenLocation());
    next();
    ParserFunctionInfo<TreeBuilder> functionInfo;
    SourceParseMode parseMode = SourceParseMode::AsyncFunctionMode;
    if (consume(TIMES))
        parseMode = SourceParseMode::AsyncGeneratorWrapperFunctionMode;
    SetForScope innerParseMode(m_parseMode, parseMode);

    FunctionNameRequirements requirements = FunctionNameRequirements::Named;
    if (declarationDefaultContext == DeclarationDefaultContext::ExportDefault) {
        // Under the "export default" context, function declaration does not require the function name.
        //
        //     ExportDeclaration:
        //         ...
        //         export default HoistableDeclaration[~Yield, +Default]
        //         ...
        //
        //     HoistableDeclaration[Yield, Default]:
        //         FunctionDeclaration[?Yield, ?Default]
        //         GeneratorDeclaration[?Yield, ?Default]
        //
        //     FunctionDeclaration[Yield, Default]:
        //         ...
        //         [+Default] function ( FormalParameters[~Yield] ) { FunctionBody[~Yield] }
        //
        //     GeneratorDeclaration[Yield, Default]:
        //         ...
        //         [+Default] function * ( FormalParameters[+Yield] ) { GeneratorBody }
        //
        // In this case, we use "*default*" as this function declaration's name.
        requirements = FunctionNameRequirements::None;
        functionInfo.name = &m_vm.propertyNames->starDefaultPrivateName;
    }

    failIfFalse((parseFunctionInfo(context, requirements, true, ConstructorKind::None, SuperBinding::NotNeeded, functionStart, functionInfo, FunctionDefinitionType::Declaration, functionConstructorParametersEndPosition)), "Cannot parse this async function");
    failIfFalse(functionInfo.name, "Async function statements must have a name");

    std::pair<DeclarationResultMask, ScopeRef> functionDeclaration = declareFunction(functionInfo.name);
    DeclarationResultMask declarationResult = functionDeclaration.first;
    failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare an async function named '", functionInfo.name->impl(), "' in strict mode");
    semanticFailIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare an async function that shadows a let/const/class/function variable '", functionInfo.name->impl(), "'");
    if (exportType == ExportType::Exported) {
        semanticFailIfFalse(exportName(*functionInfo.name), "Cannot export a duplicate function name: '", functionInfo.name->impl(), "'");
        m_moduleScopeData->exportBinding(*functionInfo.name);
    }

    TreeStatement result = context.createFuncDeclStatement(location, functionInfo);
    if (TreeBuilder::CreatesAST)
        functionDeclaration.second->appendFunction(getMetadata(functionInfo));
    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseClassDeclaration(TreeBuilder& context, ExportType exportType, DeclarationDefaultContext declarationDefaultContext)
{
    ASSERT(match(CLASSTOKEN));
    JSTokenLocation location(tokenLocation());
    JSTextPosition classStart = tokenStartPosition();
    unsigned classStartLine = tokenLine();

    ParserClassInfo<TreeBuilder> info;
    FunctionNameRequirements requirements = FunctionNameRequirements::Named;
    if (declarationDefaultContext == DeclarationDefaultContext::ExportDefault) {
        // Under the "export default" context, class declaration does not require the class name.
        //
        //     ExportDeclaration:
        //         ...
        //         export default ClassDeclaration[~Yield, +Default]
        //         ...
        //
        //     ClassDeclaration[Yield, Default]:
        //         ...
        //         [+Default] class ClassTail[?Yield]
        //
        // In this case, we use "*default*" as this class declaration's name.
        requirements = FunctionNameRequirements::None;
        info.className = &m_vm.propertyNames->starDefaultPrivateName;
    }

    TreeClassExpression classExpr = parseClass(context, requirements, info);
    failIfFalse(classExpr, "Failed to parse class");
    ASSERT(info.className);

    DeclarationResultMask declarationResult = declareVariable(info.className, DeclarationType::LetDeclaration);
    semanticFailIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare a class twice: '", info.className->impl(), "'");
    if (exportType == ExportType::Exported) {
        ASSERT_WITH_MESSAGE(declarationDefaultContext != DeclarationDefaultContext::ExportDefault, "Export default case will export the name and binding in the caller.");
        semanticFailIfFalse(exportName(*info.className), "Cannot export a duplicate class name: '", info.className->impl(), "'");
        m_moduleScopeData->exportBinding(*info.className);
    }

    JSTextPosition classEnd = lastTokenEndPosition();
    unsigned classEndLine = tokenLine();

    return context.createClassDeclStatement(location, classExpr, classStart, classEnd, classStartLine, classEndLine);
}

static constexpr ASCIILiteral instanceComputedNamePrefix { "instanceComputedName"_s };
static constexpr ASCIILiteral staticComputedNamePrefix { "staticComputedName"_s };

template <typename LexerType>
template <class TreeBuilder> TreeClassExpression Parser<LexerType>::parseClass(TreeBuilder& context, FunctionNameRequirements requirements, ParserClassInfo<TreeBuilder>& info)
{
    ASSERT(match(CLASSTOKEN));
    JSTextPosition start = tokenStartPosition();
    JSTokenLocation location(tokenLocation());
    info.startLine = location.line;
    info.startColumn = tokenColumn();
    info.startOffset = location.startOffset;

    // We have a subtle problem here. Class heritage evaluation should find class declaration's constructor name, but should not find private name evaluation.
    // For example,
    //
    //     class A extends (
    //         class {
    //             constructor() {
    //                 print(A); // This is OK.
    //                 print(A.#test); // This is SyntaxError.
    //             }
    //         }) {
    //         static #test = 42;
    //     }
    //
    // We need to create two scopes here since private name lookup will traverse scope at linking time in CodeBlock.
    // This classHeadScope is similar to functionScope in FunctionExpression with name.
    AutoPopScopeRef classHeadScope(this, pushScope());
    classHeadScope->setIsLexicalScope();
    classHeadScope->preventVarDeclarations();
    classHeadScope->setStrictMode();
    next();
    semanticFailIfTrue(currentScope()->isStaticBlock() && match(AWAIT), "Cannot use 'await' as a class name within static block");

    ASSERT_WITH_MESSAGE(requirements != FunctionNameRequirements::Unnamed, "Currently, there is no caller that uses FunctionNameRequirements::Unnamed for class syntax.");
    ASSERT_WITH_MESSAGE(!(requirements == FunctionNameRequirements::None && !info.className), "When specifying FunctionNameRequirements::None, we need to initialize info.className with the default value in the caller side.");
    if (match(IDENT) || isAllowedIdentifierAwait(m_token)) {
        info.className = m_token.m_data.ident;
        next();
        failIfTrue(classHeadScope->declareLexicalVariable(info.className, true) & DeclarationResult::InvalidStrictMode, "'", info.className->impl(), "' is not a valid class name");
    } else if (requirements == FunctionNameRequirements::Named) [[unlikely]] {
        semanticFailIfTrue(match(OPENBRACE), "Class statements must have a name");
        semanticFailureDueToKeyword("class name");
        failDueToUnexpectedToken();
    }
    ASSERT(info.className);

    JSTextPosition divot = start;
    TreeExpression parentClass = 0;
    if (consume(EXTENDS)) {
        divot = tokenStartPosition();
        parentClass = parseMemberExpression(context);
        failIfFalse(parentClass, "Cannot parse the parent class name");
    }
    const ConstructorKind constructorKind = parentClass ? ConstructorKind::Extends : ConstructorKind::Base;

    JSTextPosition classHeadEnd = lastTokenEndPosition();
    consumeOrFail(OPENBRACE, "Expected opening '{' at the start of a class body");

    AutoPopScopeRef classScope(this, pushScope());
    classScope->setIsLexicalScope();
    classScope->preventVarDeclarations();
    classScope->setStrictMode();
    classScope->setIsClassScope();

    bool declaresPrivateMethod = false;
    bool declaresPrivateAccessor = false;
    bool declaresStaticPrivateMethod = false;
    bool declaresStaticPrivateAccessor = false;

    TreeExpression constructor = 0;
    TreePropertyList classElements = 0;
    TreePropertyList classElementsTail = 0;
    unsigned nextInstanceComputedFieldID = 0;
    unsigned nextStaticComputedFieldID = 0;
    while (!match(CLOSEBRACE)) {
        if (consume(SEMICOLON))
            continue;

        JSTokenLocation methodLocation(tokenLocation());
        unsigned functionStart = tokenStart();

        // For backwards compatibility, "static" is a non-reserved keyword in non-strict mode.
        ClassElementTag tag = ClassElementTag::Instance;
        SourceParseMode parseMode = SourceParseMode::MethodMode;
        auto type = PropertyNode::Constant;
        if (match(RESERVED_IF_STRICT) && *m_token.m_data.ident == m_vm.propertyNames->staticKeyword) {
            SavePoint savePoint = createSavePoint(context);
            next();
            if (match(OPENPAREN) || match(SEMICOLON) || match(EQUAL)) {
                // Reparse "static()" as a method or "static" as a class field.
                restoreSavePoint(context, savePoint);
            } else {
                tag = ClassElementTag::Static;
                functionStart = tokenStart();
                if (match(OPENBRACE))
                    parseMode = SourceParseMode::ClassStaticBlockMode;
            }
        }

        // FIXME: Figure out a way to share more code with parseProperty.
        const CommonIdentifiers& propertyNames = *m_vm.propertyNames;
        const Identifier* ident = &propertyNames.nullIdentifier;
        TreeExpression computedPropertyName = 0;
        bool isGetter = false;
        bool isSetter = false;
        if (consume(TIMES))
            parseMode = SourceParseMode::GeneratorWrapperMethodMode;

parseMethod:
        switch (m_token.m_type) {
        namedKeyword:
        case STRING:
            ident = m_token.m_data.ident;
            ASSERT(ident);
            next();
            break;
        case BIGINT:
            ident = m_parserArena.identifierArena().makeBigIntDecimalIdentifier(const_cast<VM&>(m_vm), *m_token.m_data.bigIntString, m_token.m_data.radix);
            failIfFalse(ident, "Cannot parse big int property name");
            next();
            break;
        case ESCAPED_KEYWORD:
        case IDENT:
            if (*m_token.m_data.ident == m_vm.propertyNames->async && !m_token.m_data.escaped) [[unlikely]] {
                if (!isGeneratorMethodParseMode(parseMode) && !isAsyncMethodParseMode(parseMode)) {
                    next();
                    // We match SEMICOLON as a special case for a field called 'async' without initializer.
                    if (match(OPENPAREN) || match(COLON) || match(SEMICOLON) || match(EQUAL) || m_lexer->hasLineTerminatorBeforeToken()) {
                        ident = &m_vm.propertyNames->async;
                        break;
                    }
                    if (consume(TIMES)) [[unlikely]]
                        parseMode = SourceParseMode::AsyncGeneratorWrapperMethodMode;
                    else
                        parseMode = SourceParseMode::AsyncMethodMode;
                    goto parseMethod;
                }
            }
            [[fallthrough]];
        case AWAIT: {
            ident = m_token.m_data.ident;
            bool escaped = m_token.m_data.escaped;
            ASSERT(ident);
            next();
            if (parseMode == SourceParseMode::MethodMode && !escaped && (matchIdentifierOrKeyword() || match(STRING) || match(DOUBLE) || match(INTEGER) || match(BIGINT) || match(OPENBRACKET) || match(PRIVATENAME))) {
                isGetter = *ident == propertyNames.get;
                isSetter = *ident == propertyNames.set;
            }
            break;
        }
        case DOUBLE:
        case INTEGER:
            ident = &m_parserArena.identifierArena().makeNumericIdentifier(const_cast<VM&>(m_vm), m_token.m_data.doubleValue);
            ASSERT(ident);
            next();
            break;
        case OPENBRACKET:
            next();
            semanticFailIfTrue(currentScope()->isStaticBlock() && match(IDENT) && isArgumentsIdentifier(), "Cannot use 'arguments' as an identifier in static block");
            computedPropertyName = parseAssignmentExpression(context);
            type = static_cast<PropertyNode::Type>(type | PropertyNode::Computed);
            failIfFalse(computedPropertyName, "Cannot parse computed property name");
            handleProductionOrFail(CLOSEBRACKET, "]", "end", "computed property name");
            break;
        case PRIVATENAME: {
            ident = m_token.m_data.ident;
            failIfTrue(isGetter || isSetter, "Cannot parse class method with private name");
            ASSERT(ident);
            next();
            if (match(OPENPAREN)) {
                semanticFailIfTrue(classScope->declarePrivateMethod(*ident, tag) & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare private method twice");
                semanticFailIfTrue(*ident == propertyNames.constructorPrivateField, "Cannot declare a private method named '#constructor'");

                if (tag == ClassElementTag::Static)
                    declaresStaticPrivateMethod = true;
                else
                    declaresPrivateMethod = true;

                type = static_cast<PropertyNode::Type>(type | PropertyNode::PrivateMethod);
                break;
            }

            failIfTrue(match(OPENPAREN), "Cannot parse class method with private name");
            semanticFailIfTrue(classScope->declarePrivateField(*ident) & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare private field twice");
            type = static_cast<PropertyNode::Type>(type | PropertyNode::PrivateField);
            break;
        }
        case OPENBRACE:
            failIfFalse(parseMode == SourceParseMode::ClassStaticBlockMode, "Cannot parse static block without 'static'");
            type = static_cast<PropertyNode::Type>(type | PropertyNode::Block);
            break;
        default:
            if (m_token.m_type & KeywordTokenFlag) [[likely]]
                goto namedKeyword;
            failDueToUnexpectedToken();
        }

        TreeProperty property;
        if (isGetter || isSetter) {
            if (match(PRIVATENAME)) {
                ident = m_token.m_data.ident;

                auto declarationResult = isSetter ? classScope->declarePrivateSetter(*ident, tag) : classScope->declarePrivateGetter(*ident, tag);
                semanticFailIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Declared private setter with an already used name");
                if (tag == ClassElementTag::Static) {
                    semanticFailIfTrue(declarationResult & DeclarationResult::InvalidPrivateStaticNonStatic, "Cannot declare a private static ", (isSetter ? "setter" : "getter")  , " if there is a non-static private ", (isSetter ? "getter" : "setter"), " with used name");
                    declaresStaticPrivateAccessor = true;
                } else {
                    semanticFailIfTrue(declarationResult & DeclarationResult::InvalidPrivateStaticNonStatic, "Cannot declare a private non-static ", (isSetter ? "setter" : "getter"), " if there is a static private ", (isSetter ? "getter" : "setter"), " with used name");
                    declaresPrivateAccessor = true;
                }

                if (isSetter)
                    type = static_cast<PropertyNode::Type>(type | PropertyNode::PrivateSetter);
                else 
                    type = static_cast<PropertyNode::Type>(type | PropertyNode::PrivateGetter);
            } else {
                type = static_cast<PropertyNode::Type>(type & ~PropertyNode::Constant);
                type = static_cast<PropertyNode::Type>(type | (isGetter ? PropertyNode::Getter : PropertyNode::Setter));
            }
            property = parseGetterSetter(context, type, functionStart, ConstructorKind::None, tag);
            failIfFalse(property, "Cannot parse this method");
        } else if (!match(OPENPAREN) && parseMode == SourceParseMode::MethodMode) {
            ASSERT(!isGetter && !isSetter);
            if (ident) {
                semanticFailIfTrue(*ident == propertyNames.constructor, "Cannot declare class field named 'constructor'");
                semanticFailIfTrue(*ident == propertyNames.constructorPrivateField, "Cannot declare private class field named '#constructor'");
                if (tag == ClassElementTag::Static)
                    semanticFailIfTrue(*ident == propertyNames.prototype, "Cannot declare a static field named 'prototype'");
            }

            if (computedPropertyName) {
                if (tag == ClassElementTag::Instance)
                    ident = &m_parserArena.identifierArena().makePrivateIdentifier(m_vm, instanceComputedNamePrefix, nextInstanceComputedFieldID++);
                else
                    ident = &m_parserArena.identifierArena().makePrivateIdentifier(m_vm, staticComputedNamePrefix, nextStaticComputedFieldID++);
                DeclarationResultMask declarationResult = classScope->declareLexicalVariable(ident, true);
                ASSERT_UNUSED(declarationResult, declarationResult == DeclarationResult::Valid);
                classScope->useVariable(ident, false);
                classScope->addClosedVariableCandidateUnconditionally(ident->impl());
            }

            TreeExpression initializer = 0;
            if (consume(EQUAL)) {
                size_t usedVariablesSize = currentScope()->currentUsedVariablesSize();
                currentScope()->pushUsedVariableSet();
                SetForScope overrideParsingClassFieldInitializer(m_parserState.isParsingClassFieldInitializer, true);
                SetForScope maskAsync(m_parserState.classFieldInitMasksAsync, true);
                classScope->setExpectedSuperBinding(SuperBinding::Needed);
                initializer = parseAssignmentExpression(context);
                classScope->setExpectedSuperBinding(SuperBinding::NotNeeded);
                failIfFalse(initializer, "Cannot parse initializer for class field");
                classScope->markLastUsedVariablesSetAsCaptured(usedVariablesSize);
            }
            failIfFalse(autoSemiColon(), "Expected a ';' following a class field");
            auto inferName = initializer ? InferName::Allowed : InferName::Disallowed;
            if (computedPropertyName)
                property = context.createProperty(ident, computedPropertyName, initializer, type, SuperBinding::NotNeeded, tag);
            else
                property = context.createProperty(ident, initializer, type, SuperBinding::NotNeeded, inferName, tag);
        } else if (parseMode == SourceParseMode::ClassStaticBlockMode) {
            matchOrFail(OPENBRACE, "Expected block statement for class static block");
            size_t usedVariablesSize = currentScope()->currentUsedVariablesSize();
            currentScope()->pushUsedVariableSet();
            DepthManager statementDepth(&m_statementDepth);
            m_statementDepth = 0;
            failIfFalse(parseBlockStatement(context, BlockType::StaticBlock), "Cannot parse class static block");
            auto* symbolImpl = std::bit_cast<SymbolImpl*>(m_vm.propertyNames->builtinNames().staticInitializerBlockPrivateName().impl());
            ident = &m_parserArena.identifierArena().makeIdentifier(const_cast<VM&>(m_vm), symbolImpl);
            property = context.createProperty(ident, type, SuperBinding::Needed, tag);
            classScope->markLastUsedVariablesSetAsCaptured(usedVariablesSize);
        } else {
            ParserFunctionInfo<TreeBuilder> methodInfo;
            bool isConstructor = tag == ClassElementTag::Instance && *ident == propertyNames.constructor;
            semanticFailIfTrue(isConstructor && parseMode != SourceParseMode::MethodMode,
                "Cannot declare ", stringArticleForFunctionMode(parseMode), stringForFunctionMode(parseMode), " named 'constructor'");

            methodInfo.name = isConstructor ? info.className : ident;
            SetForScope innerParseMode(m_parseMode, parseMode);
            failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::Unnamed, false, isConstructor ? constructorKind : ConstructorKind::None, SuperBinding::Needed, functionStart, methodInfo, FunctionDefinitionType::Method)), "Cannot parse this method");

            TreeExpression method = context.createMethodDefinition(methodLocation, methodInfo);
            if (isConstructor) {
                semanticFailIfTrue(constructor, "Cannot declare multiple constructors in a single class");
                constructor = method;
                continue;
            }

            semanticFailIfTrue(tag == ClassElementTag::Static && methodInfo.name && *methodInfo.name == propertyNames.prototype,
                "Cannot declare a static method named 'prototype'");

            if (computedPropertyName)
                property = context.createProperty(computedPropertyName, method, type, SuperBinding::Needed, tag);
            else
                property = context.createProperty(methodInfo.name, method, type, SuperBinding::Needed, InferName::Allowed, tag);
        }

        if (classElementsTail)
            classElementsTail = context.createPropertyList(methodLocation, property, classElementsTail);
        else
            classElements = classElementsTail = context.createPropertyList(methodLocation, property);
    }

    info.endOffset = tokenLocation().endOffset - 1;
    consumeOrFail(CLOSEBRACE, "Expected a closing '}' after a class body");

    if (declaresPrivateMethod || declaresPrivateAccessor || declaresStaticPrivateMethod || declaresStaticPrivateAccessor) {
        {
            Identifier privateBrandIdentifier = m_vm.propertyNames->builtinNames().privateBrandPrivateName();
            DeclarationResultMask declarationResult = classScope->declareLexicalVariable(&privateBrandIdentifier, true);
            ASSERT_UNUSED(declarationResult, declarationResult == DeclarationResult::Valid);
            classScope->useVariable(&privateBrandIdentifier, false);
            classScope->addClosedVariableCandidateUnconditionally(privateBrandIdentifier.impl());
        }
        {
            Identifier privateClassBrandIdentifier = m_vm.propertyNames->builtinNames().privateClassBrandPrivateName();
            DeclarationResultMask declarationResult = classScope->declareLexicalVariable(&privateClassBrandIdentifier, true);
            ASSERT_UNUSED(declarationResult, declarationResult == DeclarationResult::Valid);
            classScope->useVariable(&privateClassBrandIdentifier, false);
            classScope->addClosedVariableCandidateUnconditionally(privateClassBrandIdentifier.impl());
        }
    }

    if constexpr (std::is_same_v<TreeBuilder, ASTBuilder>) {
        if (classElements)
            classElements->setHasPrivateAccessors(declaresPrivateAccessor || declaresStaticPrivateAccessor);
    }

    auto [lexicalEnvironment, functionDeclarations] = popScope(classScope, TreeBuilder::NeedsFreeVariableInfo);
    auto [classHeadEnvironment, classHeadFunctionDeclarations] = popScope(classHeadScope, TreeBuilder::NeedsFreeVariableInfo);
    ASSERT(functionDeclarations.isEmpty());
    ASSERT(classHeadFunctionDeclarations.isEmpty());
    return context.createClassExpr(location, info, WTFMove(classHeadEnvironment), WTFMove(lexicalEnvironment), constructor, parentClass, classElements, start, divot, classHeadEnd);
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseClassFieldInitializerSourceElements(TreeBuilder& context, const FixedVector<UnlinkedFunctionExecutable::ClassElementDefinition>& classElementDefinitions)
{
    using Kind = UnlinkedFunctionExecutable::ClassElementDefinition::Kind;

    TreeSourceElements sourceElements = context.createSourceElements();
    currentScope()->setIsClassScope();

    // Clear errors from parsing anything before the initializer expressions.
    m_lexer->clearErrorCodeAndBuffers();

    for (auto definition : classElementDefinitions) {
        auto position = definition.position;
        bool hasLineTerminatorBeforeToken = false;

        TreeStatement statement;
        if (definition.kind == Kind::StaticInitializationBlock) {
            restoreLexerState(LexerState { position.offset, static_cast<unsigned>(position.lineStartOffset), static_cast<unsigned>(position.line), static_cast<unsigned>(position.line), hasLineTerminatorBeforeToken });
            JSTokenLocation startLocation(tokenLocation());
            JSTextPosition startPosition = tokenStartPosition();
            unsigned expressionStart = tokenStart();

            ASSERT(match(RESERVED_IF_STRICT) && *m_token.m_data.ident == m_vm.propertyNames->staticKeyword);
            next();

            ParserFunctionInfo<TreeBuilder> functionInfo;
            functionInfo.name = &m_vm.propertyNames->nullIdentifier;
            SetForScope setInnerParseMode(m_parseMode, SourceParseMode::ClassStaticBlockMode);
            failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::None, false, ConstructorKind::None, SuperBinding::Needed, expressionStart, functionInfo, FunctionDefinitionType::Expression)), "Cannot parse static block function");
            TreeExpression expression = context.createFunctionExpr(startLocation, functionInfo);

            expression = context.makeStaticBlockFunctionCallNode(startLocation, expression, lastTokenEndPosition(), startPosition, lastTokenEndPosition());
            statement = context.createExprStatement(startLocation, expression, startPosition, m_lastTokenEndPosition.line);
        } else {
            JSTokenLocation location;
            location.line = position.line;
            location.lineStartOffset = position.lineStartOffset;
            location.startOffset = position.offset;

            TreeExpression initializer = 0;
            if (auto initializerPosition = definition.initializerPosition) {
                restoreLexerState(LexerState { initializerPosition->offset, static_cast<unsigned>(initializerPosition->lineStartOffset), static_cast<unsigned>(initializerPosition->line), static_cast<unsigned>(initializerPosition->line), hasLineTerminatorBeforeToken });
                // parseExpression() is more permissive way to parse AssignmentExpression than parseAssignmentExpression() that is used in parseClass().
                // This is very intentional: we need to fail for `foo = 1, 2` but support reparsing `foo = (1, 2)`, which is tricky because open paren
                // is skipped (meaning start offset points to `1`) by parsePrimaryExpression().
                initializer = parseExpression(context);
                failIfFalse(initializer, "Cannot parse expression statement");
            }

            DefineFieldNode::Type type = DefineFieldNode::Type::Name;
            if (definition.kind == Kind::FieldWithComputedPropertyKey)
                type = DefineFieldNode::Type::ComputedName;
            else if (definition.kind == Kind::FieldWithPrivatePropertyKey) {
                type = DefineFieldNode::Type::PrivateName;
                currentScope()->useVariable(definition.ident.impl(), false);
            }

            statement = context.createDefineField(location, definition.ident, initializer, type);
        }

        context.appendStatement(sourceElements, statement);
    }

    ASSERT(!hasError());
    // Trick parseInner() into believing we've parsed the entire SourceCode, in order to prevent it from producing an error.
    m_token.m_type = EOFTOK;
    return sourceElements;
}

struct LabelInfo {
    LabelInfo(const Identifier* ident, const JSTextPosition& start, const JSTextPosition& end)
    : m_ident(ident)
    , m_start(start)
    , m_end(end)
    {
    }
    
    const Identifier* m_ident;
    JSTextPosition m_start;
    JSTextPosition m_end;
};

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseExpressionOrLabelStatement(TreeBuilder& context, bool allowFunctionDeclarationAsStatement)
{
    
    /* Expression and Label statements are ambiguous at LL(1), so we have a
     * special case that looks for a colon as the next character in the input.
     */
    Vector<LabelInfo, 4> labels;
    JSTokenLocation location;
    do {
        if (!nextTokenIsColon()) {
            // If we hit this path we're making a expression statement, which
            // by definition can't make use of continue/break so we can just
            // ignore any labels we might have accumulated.
            return parseExpressionStatement(context);
        }

        semanticFailIfTrue(isPossiblyEscapedLet(m_token) && strictMode(), "Cannot use 'let' as a label ", disallowedIdentifierLetReason());
        semanticFailIfTrue(isDisallowedIdentifierAwait(m_token), "Cannot use 'await' as a label ", disallowedIdentifierAwaitReason());
        semanticFailIfTrue(isDisallowedIdentifierYield(m_token), "Cannot use 'yield' as a label ", disallowedIdentifierYieldReason());

        const Identifier* ident = m_token.m_data.ident;
        JSTextPosition start = tokenStartPosition();
        JSTextPosition end = tokenEndPosition();
        location = tokenLocation();
        next();
        consumeOrFail(COLON, "Labels must be followed by a ':'");

        // This is O(N^2) over the current list of consecutive labels, but I
        // have never seen more than one label in a row in the real world.
        for (size_t i = 0; i < labels.size(); i++)
            failIfTrue(ident->impl() == labels[i].m_ident->impl(), "Attempted to redeclare the label '", ident->impl(), "'");
        failIfTrue(getLabel(ident), "Cannot find scope for the label '", ident->impl(), "'");
        labels.append(LabelInfo(ident, start, end));
    } while (matchSpecIdentifier());
    bool isLoop = false;
    switch (m_token.m_type) {
    case FOR:
    case WHILE:
    case DO:
        isLoop = true;
        break;
        
    default:
        break;
    }
    const Identifier* unused = nullptr;
    ScopeRef labelScope = currentScope();
    for (auto& label : labels)
        pushLabel(label.m_ident, isLoop);
    m_immediateParentAllowsFunctionDeclarationInStatement = allowFunctionDeclarationAsStatement;
    TreeStatement statement = parseStatement(context, unused);
    for (size_t i = 0; i < labels.size(); i++)
        popLabel(labelScope);
    failIfFalse(statement, "Cannot parse statement");
    for (size_t i = 0; i < labels.size(); i++) {
        const LabelInfo& info = labels[labels.size() - i - 1];
        statement = context.createLabelStatement(location, info.m_ident, statement, info.m_start, info.m_end);
    }
    return statement;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseExpressionStatement(TreeBuilder& context)
{
    switch (m_token.m_type) {
    // https://tc39.es/ecma262/#sec-expression-statement
    // Despite the spec's requirement to fail from FUNCTION token here, Annex B.3.1
    // permits a labelled FunctionDeclaration in sloppy mode for web compatibility
    // reasons. We implement this semantics in parseStatement().
    case CLASSTOKEN:
        failWithMessage("'class' declaration is not directly within a block statement");
    case LET: {
        SavePoint savePoint = createSavePoint(context);
        next();
        failIfTrue(match(OPENBRACKET), "Cannot use lexical declaration in single-statement context");
        restoreSavePoint(context, savePoint);
        break;
    }
    case IDENT:
        if (*m_token.m_data.ident == m_vm.propertyNames->async && !m_token.m_data.escaped) [[unlikely]] {
            SavePoint savePoint = createSavePoint(context);
            next();
            failIfTrue(match(FUNCTION) && !m_lexer->hasLineTerminatorBeforeToken(), "Cannot use async function declaration in single-statement context");
            restoreSavePoint(context, savePoint);
        }
        break;
    default:
        break;
    }

    JSTextPosition start = tokenStartPosition();
    JSTokenLocation location(tokenLocation());
    TreeExpression expression = parseExpression(context);
    failIfFalse(expression, "Cannot parse expression statement");
    if (!autoSemiColon()) [[unlikely]]
        failDueToUnexpectedToken();
    return context.createExprStatement(location, expression, start, m_lastTokenEndPosition.line);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseIfStatement(TreeBuilder& context)
{
    ASSERT(match(IF));
    JSTokenLocation ifLocation(tokenLocation());
    int start = tokenLine();
    next();
    handleProductionOrFail2(OPENPAREN, "(", "start", "'if' condition");

    TreeExpression condition = parseExpression(context);
    failIfFalse(condition, "Expected an expression as the condition for an if statement");
    recordPauseLocation(context.breakpointLocation(condition));
    int end = tokenLine();
    handleProductionOrFail2(CLOSEPAREN, ")", "end", "'if' condition");

    const Identifier* unused = nullptr;
    m_immediateParentAllowsFunctionDeclarationInStatement = true;
    TreeStatement trueBlock = parseStatement(context, unused);
    failIfFalse(trueBlock, "Expected a statement as the body of an if block");

    if (!match(ELSE))
        return context.createIfStatement(ifLocation, condition, trueBlock, 0, start, end);

    Vector<std::tuple<TreeExpression, int, int, JSTokenLocation>, 8> exprStack;
    Vector<TreeStatement, 8> statementStack;
    bool trailingElse = false;
    do {
        JSTokenLocation tempLocation = tokenLocation();
        next();
        if (!match(IF)) {
            const Identifier* unused = nullptr;
            m_immediateParentAllowsFunctionDeclarationInStatement = true;
            TreeStatement block = parseStatement(context, unused);
            failIfFalse(block, "Expected a statement as the body of an else block");
            statementStack.append(block);
            trailingElse = true;
            break;
        }
        int innerStart = tokenLine();
        next();
        
        handleProductionOrFail2(OPENPAREN, "(", "start", "'if' condition");

        TreeExpression innerCondition = parseExpression(context);
        failIfFalse(innerCondition, "Expected an expression as the condition for an if statement");
        recordPauseLocation(context.breakpointLocation(innerCondition));
        int innerEnd = tokenLine();
        handleProductionOrFail2(CLOSEPAREN, ")", "end", "'if' condition");
        const Identifier* unused = nullptr;
        m_immediateParentAllowsFunctionDeclarationInStatement = true;
        TreeStatement innerTrueBlock = parseStatement(context, unused);
        failIfFalse(innerTrueBlock, "Expected a statement as the body of an if block");
        exprStack.append(std::tuple { innerCondition, innerStart, innerEnd, tempLocation });
        statementStack.append(innerTrueBlock);
    } while (match(ELSE));

    if (!trailingElse) {
        auto [condition, start, end, location] = exprStack.takeLast();
        TreeStatement trueBlock = statementStack.takeLast();
        TreeStatement ifStatement = context.createIfStatement(location, condition, trueBlock, 0, start, end);
        context.setEndOffset(ifStatement, context.endOffset(trueBlock));
        statementStack.append(ifStatement);
    }

    while (!exprStack.isEmpty()) {
        auto [condition, start, end, location] = exprStack.takeLast();
        TreeStatement falseBlock = statementStack.takeLast();
        TreeStatement trueBlock = statementStack.takeLast();
        TreeStatement ifStatement = context.createIfStatement(location, condition, trueBlock, falseBlock, start, end);
        context.setEndOffset(ifStatement, context.endOffset(falseBlock));
        statementStack.append(ifStatement);
    }

    return context.createIfStatement(ifLocation, condition, trueBlock, statementStack.last(), start, end);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::ModuleName Parser<LexerType>::parseModuleName(TreeBuilder& context)
{
    // ModuleName (ModuleSpecifier in the spec) represents the module name imported by the script.
    // http://www.ecma-international.org/ecma-262/6.0/#sec-imports
    // http://www.ecma-international.org/ecma-262/6.0/#sec-exports
    JSTokenLocation specifierLocation(tokenLocation());
    failIfFalse(match(STRING), "Imported modules names must be string literals");
    const Identifier* moduleName = m_token.m_data.ident;
    next();
    return context.createModuleName(specifierLocation, *moduleName);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::ImportSpecifier Parser<LexerType>::parseImportClauseItem(TreeBuilder& context, ImportSpecifierType specifierType)
{
    // Produced node is the item of the ImportClause.
    // That is the ImportSpecifier, ImportedDefaultBinding or NameSpaceImport.
    // http://www.ecma-international.org/ecma-262/6.0/#sec-imports
    JSTokenLocation specifierLocation(tokenLocation());
    JSToken localNameToken;
    const Identifier* importedName = nullptr;
    const Identifier* localName = nullptr;

    switch (specifierType) {
    case ImportSpecifierType::NamespaceImport: {
        // NameSpaceImport :
        // * as ImportedBinding
        // e.g.
        //     * as namespace
        ASSERT(match(TIMES));
        importedName = &m_vm.propertyNames->timesIdentifier;
        next();

        failIfFalse(matchContextualKeyword(m_vm.propertyNames->as), "Expected 'as' before imported binding name");
        next();

        failIfFalse(matchSpecIdentifier(), "Expected a variable name for the import declaration");
        localNameToken = m_token;
        localName = m_token.m_data.ident;
        next();
        break;
    }

    case ImportSpecifierType::NamedImport: {
        // ImportSpecifier :
        // ImportedBinding
        // IdentifierName as ImportedBinding
        // ModuleExportName as ImportedBinding
        // e.g.
        //     A
        //     A as B
        ASSERT(matchIdentifierOrKeyword() || match(STRING));
        bool isModuleExportName = match(STRING);
        localName = m_token.m_data.ident;
        importedName = localName;
        localNameToken = m_token;
        if (isModuleExportName)
            failIfTrue(hasUnpairedSurrogate(localName->string()), "Expected a well-formed-unicode string for the module export name");
        next();

        bool useAs = matchContextualKeyword(m_vm.propertyNames->as);
        if (isModuleExportName)
            failIfFalse(useAs, "Expected 'as' after the module export name string");
        if (useAs) {
            next();
            failIfFalse(matchSpecIdentifier(), "Expected a variable name for the import declaration");
            localNameToken = m_token;
            localName = m_token.m_data.ident;
            next();
        }
        break;
    }

    case ImportSpecifierType::DefaultImport: {
        // ImportedDefaultBinding :
        // ImportedBinding
        ASSERT(matchSpecIdentifier());
        localNameToken = m_token;
        localName = m_token.m_data.ident;
        importedName = &m_vm.propertyNames->defaultKeyword;
        next();
        break;
    }
    }

    semanticFailIfTrue(localNameToken.m_type == AWAIT, "Cannot use 'await' as an imported binding name");
    semanticFailIfTrue(localNameToken.m_type & KeywordTokenFlag, "Cannot use keyword as imported binding name");
    DeclarationResultMask declarationResult = declareVariable(localName, DeclarationType::ConstDeclaration, (specifierType == ImportSpecifierType::NamespaceImport) ? DeclarationImportType::ImportedNamespace : DeclarationImportType::Imported);
    if (declarationResult != DeclarationResult::Valid) {
        failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare an imported binding named ", localName->impl(), " in strict mode");
        semanticFailIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare an imported binding name twice: '", localName->impl(), "'");
    }

    return context.createImportSpecifier(specifierLocation, *importedName, *localName);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::ImportAttributesList Parser<LexerType>::parseImportAttributes(TreeBuilder& context)
{
    UncheckedKeyHashSet<UniquedStringImpl*> keys;
    auto attributesList = context.createImportAttributesList();
    consumeOrFail(OPENBRACE, "Expected opening '{' at the start of import attribute");
    while (!match(CLOSEBRACE)) {
        failIfFalse(matchIdentifierOrKeyword() || match(STRING), "Expected an attribute key");
        auto key = m_token.m_data.ident;
        failIfFalse(keys.add(key->impl()).isNewEntry, "A duplicate key for import attributes '", key->impl(), "'");
        next();
        consumeOrFail(COLON, "Expected ':' after attribute key");
        failIfFalse(match(STRING), "Expected an attribute value");
        auto value = m_token.m_data.ident;
        next();
        context.appendImportAttribute(attributesList, *key, *value);
        if (!consume(COMMA))
            break;
    }
    handleProductionOrFail2(CLOSEBRACE, "}", "end", "import attribute");
    return attributesList;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseImportDeclaration(TreeBuilder& context)
{
    // http://www.ecma-international.org/ecma-262/6.0/#sec-imports
    ASSERT(match(IMPORT));
    JSTokenLocation importLocation(tokenLocation());
    next();

    auto specifierList = context.createImportSpecifierList();
    auto type = ImportDeclarationNode::ImportType::Normal;

    if (match(STRING)) {
        // import ModuleSpecifier ;
        // import ModuleSpecifier [no LineTerminator here] WithClause ;
        auto moduleName = parseModuleName(context);
        failIfFalse(moduleName, "Cannot parse the module name");

        typename TreeBuilder::ImportAttributesList attributesList = 0;
        if (!m_lexer->hasLineTerminatorBeforeToken() && match(WITH)) {
            next();
            attributesList = parseImportAttributes(context);
            failIfFalse(attributesList, "Unable to parse import attributes");
        }

        failIfFalse(autoSemiColon(), "Expected a ';' following a targeted import declaration");
        return context.createImportDeclaration(importLocation, type, specifierList, moduleName, attributesList);
    }

    bool isFinishedParsingImport = false;
    bool hasImportDefer = false;
    if (Options::useImportDefer() && matchContextualKeyword(m_vm.propertyNames->deferKeyword)) [[unlikely]] {
        SavePoint deferSavePoint = createSavePoint(context);
        next();
        if (match(TIMES)) {
            // import defer NameSpaceImport FromClause ;
            type = ImportDeclarationNode::ImportType::Deferred;
            hasImportDefer = true;
        } else
            // import defer FromClause ;
            restoreSavePoint(context, deferSavePoint);
    }

    if (matchSpecIdentifier() && !hasImportDefer) {
        // ImportedDefaultBinding :
        // ImportedBinding
        auto specifier = parseImportClauseItem(context, ImportSpecifierType::DefaultImport);
        failIfFalse(specifier, "Cannot parse the default import");
        context.appendImportSpecifier(specifierList, specifier);
        if (match(COMMA))
            next();
        else
            isFinishedParsingImport = true;
    }

    if (!isFinishedParsingImport) {
        if (match(TIMES)) {
            // import NameSpaceImport FromClause ;
            auto specifier = parseImportClauseItem(context, ImportSpecifierType::NamespaceImport);
            failIfFalse(specifier, "Cannot parse the namespace import");
            context.appendImportSpecifier(specifierList, specifier);
        } else {
            consumeOrFail(OPENBRACE, "Expected namespace import or import list");
            // NamedImports :
            // { }
            // { ImportsList }
            // { ImportsList , }
            while (!match(CLOSEBRACE)) {
                failIfFalse(matchIdentifierOrKeyword() || match(STRING), "Expected an imported name or a module export name string for the import declaration");
                auto specifier = parseImportClauseItem(context, ImportSpecifierType::NamedImport);
                failIfFalse(specifier, "Cannot parse the named import");
                context.appendImportSpecifier(specifierList, specifier);
                if (!consume(COMMA))
                    break;
            }
            handleProductionOrFail2(CLOSEBRACE, "}", "end", "import list");
        }
    }

    // FromClause :
    // from ModuleSpecifier

    failIfFalse(matchContextualKeyword(m_vm.propertyNames->from), "Expected 'from' before imported module name");
    next();

    auto moduleName = parseModuleName(context);
    failIfFalse(moduleName, "Cannot parse the module name");

    // [no LineTerminator here] WithClause ;
    typename TreeBuilder::ImportAttributesList attributesList = 0;
    if (!m_lexer->hasLineTerminatorBeforeToken() && match(WITH)) {
        next();
        attributesList = parseImportAttributes(context);
        failIfFalse(attributesList, "Unable to parse import attributes");
    }

    failIfFalse(autoSemiColon(), "Expected a ';' following a targeted import declaration");

    return context.createImportDeclaration(importLocation, type, specifierList, moduleName, attributesList);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::ExportSpecifier Parser<LexerType>::parseExportSpecifier(TreeBuilder& context, Vector<std::pair<const Identifier*, const Identifier*>, 8>& maybeExportedLocalNames, bool& hasKeywordForLocalBindings, bool& hasReferencedModuleExportNames)
{
    // ExportSpecifier :
    // IdentifierName
    // IdentifierName as IdentifierName
    // IdentifierName as ModuleExportName
    // ModuleExportName
    // ModuleExportName as IdentifierName
    // ModuleExportName as ModuleExportName
    // http://www.ecma-international.org/ecma-262/6.0/#sec-exports
    ASSERT(matchIdentifierOrKeyword() || match(STRING));
    JSTokenLocation specifierLocation(tokenLocation());
    const Identifier* localName = m_token.m_data.ident;
    const Identifier* exportedName = localName;
    if (match(STRING)) {
        hasReferencedModuleExportNames = true;
        failIfTrue(hasUnpairedSurrogate(exportedName->string()), "Expected a well-formed-unicode string for the module export name");
    } else {
        if (m_token.m_type & KeywordTokenFlag)
            hasKeywordForLocalBindings = true;
    }
    next();

    if (matchContextualKeyword(m_vm.propertyNames->as)) {
        next();
        failIfFalse(matchIdentifierOrKeyword() || match(STRING), "Expected an exported name or a module export name string for the export declaration");
        exportedName = m_token.m_data.ident;
        if (match(STRING))
            failIfTrue(hasUnpairedSurrogate(exportedName->string()), "Expected a well-formed-unicode string for the module export name");
        next();
    }

    semanticFailIfFalse(exportName(*exportedName), "Cannot export a duplicate name '", exportedName->impl(), "'");
    maybeExportedLocalNames.append(std::make_pair(localName, exportedName));
    return context.createExportSpecifier(specifierLocation, *localName, *exportedName);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseExportDeclaration(TreeBuilder& context)
{
    // http://www.ecma-international.org/ecma-262/6.0/#sec-exports
    ASSERT(match(EXPORT_));
    JSTokenLocation exportLocation(tokenLocation());
    next();

    switch (m_token.m_type) {
    case TIMES: {
        // export * FromClause ;
        // export * as IdentifierName FromClause ;
        // export * as ModuleExportName FromClause ;
        next();

        const Identifier* exportedName = nullptr;
        JSTokenLocation specifierLocation;
        if (matchContextualKeyword(m_vm.propertyNames->as)) {
            next();
            specifierLocation = JSTokenLocation(tokenLocation());
            failIfFalse(matchIdentifierOrKeyword() || match(STRING), "Expected an exported name or a module export name string for the export declaration");
            exportedName = m_token.m_data.ident;
            if (match(STRING))
                failIfTrue(hasUnpairedSurrogate(exportedName->string()), "Expected a well-formed-unicode string for the module export name");
            next();
        }

        failIfFalse(matchContextualKeyword(m_vm.propertyNames->from), "Expected 'from' before exported module name");
        next();
        auto moduleName = parseModuleName(context);
        failIfFalse(moduleName, "Cannot parse the 'from' clause");

        // [no LineTerminator here] WithClause ;
        typename TreeBuilder::ImportAttributesList attributesList = 0;
        if (!m_lexer->hasLineTerminatorBeforeToken() && match(WITH)) {
            next();
            attributesList = parseImportAttributes(context);
            failIfFalse(attributesList, "Unable to parse import attributes");
        }

        failIfFalse(autoSemiColon(), "Expected a ';' following a targeted export declaration");

        if (exportedName) {
            semanticFailIfFalse(exportName(*exportedName), "Cannot export a duplicate name '", exportedName->impl(), "'");
            auto specifierList = context.createExportSpecifierList();
            auto localName = &m_vm.propertyNames->starNamespacePrivateName;
            auto specifier = context.createExportSpecifier(specifierLocation, *localName, *exportedName);
            context.appendExportSpecifier(specifierList, specifier);
            return context.createExportNamedDeclaration(exportLocation, specifierList, moduleName, attributesList);
        }

        return context.createExportAllDeclaration(exportLocation, moduleName, attributesList);
    }

    case DEFAULT: {
        // export default HoistableDeclaration[~Yield, ~Await, +Default]
        // export default ClassDeclaration[~Yield, ~Await, +Default]
        // export default [lookahead not-in { function, async [no LineTerminator here] function, class }] AssignmentExpression[+In, ~Yield, ~Await]

        next();

        TreeStatement result = 0;
        bool isFunctionOrClassDeclaration = false;
        const Identifier* localName = nullptr;

        bool startsWithFunction = match(FUNCTION);
        if (startsWithFunction || match(CLASSTOKEN)) {
            SavePoint savePoint = createSavePoint(context);
            isFunctionOrClassDeclaration = true;
            next();

            // ES6 Generators
            if (startsWithFunction && match(TIMES))
                next();
            if (match(IDENT))
                localName = m_token.m_data.ident;
            restoreSavePoint(context, savePoint);
        } else if (matchContextualKeyword(m_vm.propertyNames->async)) {
            // export default async function xxx() { }
            // export default async function * yyy() { }
            SavePoint savePoint = createSavePoint(context);
            next();
            if (match(FUNCTION) && !m_lexer->hasLineTerminatorBeforeToken()) {
                next();
                // Async Generators
                consume(TIMES);
                if (match(IDENT))
                    localName = m_token.m_data.ident;
                isFunctionOrClassDeclaration = true;
            }
            restoreSavePoint(context, savePoint);
        }

        if (!localName)
            localName = &m_vm.propertyNames->starDefaultPrivateName;

        if (isFunctionOrClassDeclaration) {
            if (startsWithFunction) {
                ASSERT(match(FUNCTION));
                DepthManager statementDepth(&m_statementDepth);
                m_statementDepth = 1;
                result = parseFunctionDeclaration(context, FunctionDeclarationType::Declaration, ExportType::NotExported, DeclarationDefaultContext::ExportDefault);
            } else if (match(CLASSTOKEN)) {
                result = parseClassDeclaration(context, ExportType::NotExported, DeclarationDefaultContext::ExportDefault);
            } else {
                ASSERT(matchContextualKeyword(m_vm.propertyNames->async));
                unsigned functionStart = m_token.m_startPosition;
                next();
                DepthManager statementDepth(&m_statementDepth);
                m_statementDepth = 1;
                result = parseAsyncFunctionDeclaration(context, functionStart, ExportType::NotExported, DeclarationDefaultContext::ExportDefault);
            }
        } else {
            // export default expr;
            //
            // It should be treated as the same to the following.
            //
            // const *default* = expr;
            // export { *default* as default }
            //
            // In the above example, *default* is the invisible variable to the users.
            // We use the private symbol to represent the name of this variable.
            JSTokenLocation location(tokenLocation());
            JSTextPosition start = tokenStartPosition();
            TreeExpression expression = parseAssignmentExpression(context);
            failIfFalse(expression, "Cannot parse expression");

            DeclarationResultMask declarationResult = declareVariable(&m_vm.propertyNames->starDefaultPrivateName, DeclarationType::ConstDeclaration);
            semanticFailIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Only one 'default' export is allowed");

            TreeExpression assignment = context.createAssignResolve(location, m_vm.propertyNames->starDefaultPrivateName, expression, start, start, tokenEndPosition(), AssignmentContext::ConstDeclarationStatement);
            result = context.createExprStatement(location, assignment, start, tokenEndPosition());
            failIfFalse(autoSemiColon(), "Expected a ';' following a targeted export declaration");
        }
        failIfFalse(result, "Cannot parse the declaration");

        semanticFailIfFalse(exportName(m_vm.propertyNames->defaultKeyword), "Only one 'default' export is allowed");
        m_moduleScopeData->exportBinding(*localName, m_vm.propertyNames->defaultKeyword);
        return context.createExportDefaultDeclaration(exportLocation, result, *localName);
    }

    case OPENBRACE: {
        // export ExportClause FromClause ;
        // export ExportClause ;
        //
        // ExportClause :
        // { }
        // { ExportsList }
        // { ExportsList , }
        //
        // ExportsList :
        // ExportSpecifier
        // ExportsList , ExportSpecifier

        next();

        auto specifierList = context.createExportSpecifierList();
        Vector<std::pair<const Identifier*, const Identifier*>, 8> maybeExportedLocalNames;

        bool hasKeywordForLocalBindings = false;
        bool hasReferencedModuleExportNames = false;
        while (!match(CLOSEBRACE)) {
            failIfFalse(matchIdentifierOrKeyword() || match(STRING), "Expected a variable name or a module export name string for the export declaration");
            auto specifier = parseExportSpecifier(context, maybeExportedLocalNames, hasKeywordForLocalBindings, hasReferencedModuleExportNames);
            failIfFalse(specifier, "Cannot parse the named export");
            context.appendExportSpecifier(specifierList, specifier);
            if (!consume(COMMA))
                break;
        }
        handleProductionOrFail2(CLOSEBRACE, "}", "end", "export list");

        typename TreeBuilder::ModuleName moduleName = 0;
        typename TreeBuilder::ImportAttributesList attributesList = 0;
        if (matchContextualKeyword(m_vm.propertyNames->from)) {
            next();
            moduleName = parseModuleName(context);
            failIfFalse(moduleName, "Cannot parse the 'from' clause");

            // [no LineTerminator here] WithClause ;
            if (!m_lexer->hasLineTerminatorBeforeToken() && match(WITH)) {
                next();
                attributesList = parseImportAttributes(context);
                failIfFalse(attributesList, "Unable to parse import attributes");
            }
        } else
            semanticFailIfTrue(hasReferencedModuleExportNames, "Cannot use module export names if they reference variable names in the current module");
        failIfFalse(autoSemiColon(), "Expected a ';' following a targeted export declaration");

        if (!moduleName) {
            semanticFailIfTrue(hasKeywordForLocalBindings, "Cannot use keyword as exported variable name");
            // Since this export declaration does not have module specifier part, it exports the local bindings.
            // While the export declaration with module specifier does not have any effect on the current module's scope,
            // the export named declaration without module specifier references the local binding names.
            // For example,
            //   export { A, B, C as D } from "mod"
            // does not have effect on the current module's scope. But,
            //   export { A, B, C as D }
            // will reference the current module's bindings.
            for (const auto& pair : maybeExportedLocalNames) {
                const Identifier* localName = pair.first;
                const Identifier* exportedName = pair.second;
                m_moduleScopeData->exportBinding(*localName, *exportedName);
            }
        }

        return context.createExportNamedDeclaration(exportLocation, specifierList, moduleName, attributesList);
    }

    default: {
        // export VariableStatement
        // export Declaration
        TreeStatement result = 0;
        switch (m_token.m_type) {
        case VAR:
            result = parseVariableDeclaration(context, DeclarationType::VarDeclaration, ExportType::Exported);
            break;

        case CONSTTOKEN:
            result = parseVariableDeclaration(context, DeclarationType::ConstDeclaration, ExportType::Exported);
            break;

        case LET:
            result = parseVariableDeclaration(context, DeclarationType::LetDeclaration, ExportType::Exported);
            break;

        case FUNCTION: {
            DepthManager statementDepth(&m_statementDepth);
            m_statementDepth = 1;
            result = parseFunctionDeclaration(context, FunctionDeclarationType::Declaration, ExportType::Exported);
            break;
        }

        case CLASSTOKEN:
            result = parseClassDeclaration(context, ExportType::Exported);
            break;

        case IDENT:
            if (*m_token.m_data.ident == m_vm.propertyNames->async && !m_token.m_data.escaped) [[likely]] {
                unsigned functionStart = m_token.m_startPosition;
                next();
                semanticFailIfFalse(match(FUNCTION) && !m_lexer->hasLineTerminatorBeforeToken(), "Expected 'function' keyword following 'async' keyword with no preceding line terminator");
                DepthManager statementDepth(&m_statementDepth);
                m_statementDepth = 1;
                result = parseAsyncFunctionDeclaration(context, functionStart, ExportType::Exported);
                break;
            }
            [[fallthrough]];
        default:
            failWithMessage("Expected either a declaration or a variable statement");
        }

        failIfFalse(result, "Cannot parse the declaration");
        return context.createExportLocalDeclaration(exportLocation, result);
    }
    }

    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseExpression(TreeBuilder& context)
{
    failIfStackOverflow();
    JSTokenLocation headLocation(tokenLocation());
    TreeExpression node = parseAssignmentExpression(context);
    failIfFalse(node, "Cannot parse expression");
    context.setEndOffset(node, m_lastTokenEndPosition.offset);
    if (!match(COMMA))
        return node;
    recordPauseLocation(context.breakpointLocation(node));
    next();
    m_parserState.nonTrivialExpressionCount++;
    m_parserState.nonLHSCount++;
    JSTokenLocation tailLocation(tokenLocation());
    TreeExpression right = parseAssignmentExpression(context);
    failIfFalse(right, "Cannot parse expression in a comma expression");
    recordPauseLocation(context.breakpointLocation(right));
    context.setEndOffset(right, m_lastTokenEndPosition.offset);
    typename TreeBuilder::Comma head = context.createCommaExpr(headLocation, node);
    typename TreeBuilder::Comma tail = context.appendToCommaExpr(tailLocation, head, right);
    while (match(COMMA)) {
        next(TreeBuilder::DontBuildStrings);
        tailLocation = tokenLocation();
        right = parseAssignmentExpression(context);
        failIfFalse(right, "Cannot parse expression in a comma expression");
        context.setEndOffset(right, m_lastTokenEndPosition.offset);
        recordPauseLocation(context.breakpointLocation(right));
        tail = context.appendToCommaExpr(tailLocation, tail, right);
    }
    context.setEndOffset(head, m_lastTokenEndPosition.offset);
    return head;
}

template <typename LexerType>
template <typename TreeBuilder> TreeExpression Parser<LexerType>::parseAssignmentExpressionOrPropagateErrorClass(TreeBuilder& context)
{
    ExpressionErrorClassifier classifier(this);
    auto assignment = parseAssignmentExpression(context, classifier);
    if (!assignment)
        classifier.propagateExpressionErrorClass();
    return assignment;
}

template <typename LexerType>
template <typename TreeBuilder> TreeExpression Parser<LexerType>::parseAssignmentExpression(TreeBuilder& context)
{
    ExpressionErrorClassifier classifier(this);
    return parseAssignmentExpression(context, classifier);
}


template <typename LexerType>
template <typename TreeBuilder> NEVER_INLINE const char* Parser<LexerType>::metaPropertyName(TreeBuilder& context, TreeExpression expr)
{
    if (context.isNewTarget(expr))
        return "new.target";
    if (context.isImportMeta(expr))
        return "import.meta";
    RELEASE_ASSERT_NOT_REACHED();
    return "error";
}

template <typename LexerType>
template <typename TreeBuilder> bool Parser<LexerType>::isSimpleAssignmentTarget(TreeBuilder& context, TreeExpression expr, bool ignoreStrictCheck)
{
    // Web compatibility concerns prevent us from handling a function call LHS as an early error in sloppy mode.
    // See https://github.com/tc39/ecma262/pull/3568 for details.
    return context.isLocation(expr) || (!(strictMode() || ignoreStrictCheck) && context.isFunctionCall(expr));
}

template <typename LexerType>
template <typename TreeBuilder> TreeExpression Parser<LexerType>::parseAssignmentExpression(TreeBuilder& context, ExpressionErrorClassifier& classifier)
{
    ASSERT(!hasError());
    
    failIfStackOverflow();

    if (match(YIELD) && !canUseIdentifierYield())
        return parseYieldExpression(context);

    JSTextPosition start = tokenStartPosition();
    JSTokenLocation location(tokenLocation());
    int initialAssignmentCount = m_parserState.assignmentCount;
    int initialNonLHSCount = m_parserState.nonLHSCount;
    bool maybeAssignmentPattern = match(OPENBRACE) || match(OPENBRACKET);
    bool wasOpenParen = match(OPENPAREN);
    // Do not use matchSpecIdentifier() here since it is slower than isIdentifierOrKeyword.
    // Whether spec identifier is will be validated by isArrowFunctionParameters().
    bool wasIdentifierOrKeyword = matchIdentifierOrKeyword() || (m_token.m_type == ESCAPED_KEYWORD);
    bool maybeValidArrowFunctionStart = wasOpenParen || wasIdentifierOrKeyword;
    SavePoint savePoint = createSavePoint(context);
    size_t usedVariablesSize = 0;

    if (wasOpenParen) {
        usedVariablesSize = currentScope()->currentUsedVariablesSize();
        currentScope()->pushUsedVariableSet();
    }

    TreeExpression lhs = parseConditionalExpression(context);

    // Current implementation of parseAssignmentExpression causes a weird parsing loop 
    // for this example:
    //
    //      class C {
    //          static {
    //              ((x = await) => 0);
    //          }
    //      }
    //
    // which makes the 'await' error caught in parseConditionalExpression escaping from
    // parseAssignmentExpression. Therefore, we need to capture the error directly after
    // parseConditionalExpression. Besides, the usage of `await` is strictly limited in 
    // class static block.
    if (!lhs && currentScope()->isStaticBlock() && match(AWAIT)) [[unlikely]]
        propagateError();

    if (maybeValidArrowFunctionStart && !match(EOFTOK)) {
        bool isArrowFunctionToken = match(ARROWFUNCTION);
        if (!lhs || isArrowFunctionToken) {
            SavePointWithError errorRestorationSavePoint = swapSavePointForError(context, savePoint);
            bool isAsync { false };
            if (classifier.indicatesPossibleAsyncArrowFunction()) [[unlikely]] {
                if (matchContextualKeyword(m_vm.propertyNames->async)) {
                    isAsync = true;
                    next();
                }
            }
            if (isArrowFunctionParameters(context)) {
                if (wasOpenParen)
                    currentScope()->revertToPreviousUsedVariables(usedVariablesSize);
                return parseArrowFunctionExpression(context, isAsync, location);
            }
            if (isArrowFunctionToken)
                propagateError();
            restoreSavePointWithError(context, errorRestorationSavePoint);
            if (isArrowFunctionToken) [[unlikely]]
                failDueToUnexpectedToken();
        }
    }

    if (!lhs && (!maybeAssignmentPattern || !classifier.indicatesPossiblePattern()))
        propagateError();

    if (maybeAssignmentPattern && (!lhs || (context.isObjectOrArrayLiteral(lhs) && match(EQUAL)))) {
        SavePointWithError expressionErrorLocation = swapSavePointForError(context, savePoint);
        auto pattern = tryParseDestructuringPatternExpression(context, AssignmentContext::AssignmentExpression);
        if (classifier.indicatesPossiblePattern() && (!pattern || !match(EQUAL))) {
            restoreSavePointWithError(context, expressionErrorLocation);
            return 0;
        }
        failIfFalse(pattern, "Cannot parse assignment pattern");
        consumeOrFail(EQUAL, "Expected '=' following assignment pattern");
        auto rhs = parseAssignmentExpression(context);
        if (!rhs)
            propagateError();
        return context.createDestructuringAssignment(location, pattern, rhs);
    }

    failIfFalse(lhs, "Cannot parse expression");
    if (initialNonLHSCount != m_parserState.nonLHSCount) {
        semanticFailIfTrue(m_token.m_type >= EQUAL && m_token.m_type <= ANDEQUAL, "Left hand side of operator '", getToken(), "' must be a reference");
        return lhs;
    }

    int assignmentStack = 0;
    Operator op;
    bool hadAssignment = false;
    while (true) {
        switch (m_token.m_type) {
        case EQUAL: op = Operator::Equal; break;
        case PLUSEQUAL: op = Operator::PlusEq; break;
        case MINUSEQUAL: op = Operator::MinusEq; break;
        case MULTEQUAL: op = Operator::MultEq; break;
        case DIVEQUAL: op = Operator::DivEq; break;
        case LSHIFTEQUAL: op = Operator::LShift; break;
        case RSHIFTEQUAL: op = Operator::RShift; break;
        case URSHIFTEQUAL: op = Operator::URShift; break;
        case BITANDEQUAL: op = Operator::BitAndEq; break;
        case BITXOREQUAL: op = Operator::BitXOrEq; break;
        case BITOREQUAL: op = Operator::BitOrEq; break;
        case MODEQUAL: op = Operator::ModEq; break;
        case POWEQUAL: op = Operator::PowEq; break;
        case COALESCEEQUAL: op = Operator::CoalesceEq; break;
        case OREQUAL: op = Operator::OrEq; break;
        case ANDEQUAL: op = Operator::AndEq; break;
        default:
            goto end;
        }
        m_parserState.nonTrivialExpressionCount++;
        hadAssignment = true;
        semanticFailIfTrue(context.isMetaProperty(lhs), metaPropertyName(context, lhs), " can't be the left hand side of an assignment expression");
        // Even if in sloppy mode, we should throw a syntax error for logical assignment expressions that are not simple.
        // https://tc39.es/ecma262/#sec-assignment-operators-static-semantics-early-errors
        semanticFailIfFalse(isSimpleAssignmentTarget(context, lhs, op == Operator::CoalesceEq || op == Operator::OrEq || op == Operator::AndEq), "Left side of assignment is not a reference");
        context.assignmentStackAppend(assignmentStack, lhs, start, tokenStartPosition(), m_parserState.assignmentCount, op);
        start = tokenStartPosition();
        m_parserState.assignmentCount++;
        next(TreeBuilder::DontBuildStrings);
        if (strictMode() && m_parserState.lastIdentifier && context.isResolve(lhs)) {
            failIfTrueIfStrict(m_vm.propertyNames->eval == *m_parserState.lastIdentifier, "Cannot modify 'eval' in strict mode");
            failIfTrueIfStrict(m_vm.propertyNames->arguments == *m_parserState.lastIdentifier, "Cannot modify 'arguments' in strict mode");
            m_parserState.lastIdentifier = nullptr;
        }
        lhs = parseAssignmentExpression(context);
        failIfFalse(lhs, "Cannot parse the right hand side of an assignment expression");
        if (initialNonLHSCount != m_parserState.nonLHSCount) {
            semanticFailIfTrue(m_token.m_type >= EQUAL && m_token.m_type <= ANDEQUAL, "Left hand side of operator '", getToken(), "' must be a reference");
            break;
        }
    }
end:
    if (hadAssignment)
        m_parserState.nonLHSCount++;
    
    while (assignmentStack)
        lhs = context.createAssignment(location, assignmentStack, lhs, initialAssignmentCount, m_parserState.assignmentCount, lastTokenEndPosition());
    
    return lhs;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseYieldExpression(TreeBuilder& context)
{
    // YieldExpression[In] :
    //     yield
    //     yield [no LineTerminator here] AssignmentExpression[?In, Yield]
    //     yield [no LineTerminator here] * AssignmentExpression[?In, Yield]

    // http://ecma-international.org/ecma-262/6.0/#sec-generator-function-definitions
    failIfFalse(currentScope()->isGeneratorFunction() && !currentScope()->isArrowFunctionBoundary(), "Cannot use yield expression out of generator");

    // http://ecma-international.org/ecma-262/6.0/#sec-generator-function-definitions-static-semantics-early-errors
    failIfTrue(m_parserState.functionParsePhase == FunctionParsePhase::Parameters, "Cannot use yield expression within parameters");

    // https://github.com/tc39/ecma262/issues/3333
    failIfTrue(m_parserState.isParsingClassFieldInitializer, "Cannot use yield expression inside class field initializer expression");

    JSTokenLocation location(tokenLocation());
    JSTextPosition divotStart = tokenStartPosition();
    ASSERT(match(YIELD));
    SavePoint savePoint = createSavePoint(context);
    next();
    if (m_lexer->hasLineTerminatorBeforeToken())
        return context.createYield(location);

    bool delegate = consume(TIMES);
    JSTextPosition argumentStart = tokenStartPosition();
    TreeExpression argument = parseAssignmentExpression(context);
    if (!argument) {
        restoreSavePoint(context, savePoint);
        next();
        return context.createYield(location);
    }
    return context.createYield(location, argument, delegate, divotStart, argumentStart, lastTokenEndPosition());
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseAwaitExpression(TreeBuilder& context)
{
    ASSERT(match(AWAIT));
    ASSERT(currentScope()->isAsyncFunction() || isModuleParseMode(sourceParseMode()));
    ASSERT(isAsyncFunctionParseMode(sourceParseMode()) || isModuleParseMode(sourceParseMode()));
    ASSERT(m_parserState.functionParsePhase != FunctionParsePhase::Parameters);
    ASSERT(!m_parserState.classFieldInitMasksAsync);
    JSTokenLocation location(tokenLocation());
    JSTextPosition divotStart = tokenStartPosition();
    next();
    JSTextPosition argumentStart = tokenStartPosition();
    ExpressionErrorClassifier classifier(this);
    TreeExpression argument = parseUnaryExpression(context);
    failIfFalse(argument, "Failed to parse await expression");
    return context.createAwait(location, argument, divotStart, argumentStart, lastTokenEndPosition());
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseConditionalExpression(TreeBuilder& context)
{
    JSTokenLocation location(tokenLocation());
    TreeExpression cond = parseBinaryExpression(context);
    failIfFalse(cond, "Cannot parse expression");
    if (!match(QUESTION))
        return cond;
    m_parserState.nonTrivialExpressionCount++;
    m_parserState.nonLHSCount++;
    next(TreeBuilder::DontBuildStrings);
    TreeExpression lhs = 0;
    {
        // this block is necessary so that we don't leave `in` enabled for the rhs
        AllowInOverride allowInOverride(this);
        lhs = parseAssignmentExpression(context);
    }
    failIfFalse(lhs, "Cannot parse left hand side of ternary operator");
    context.setEndOffset(lhs, m_lastTokenEndPosition.offset);
    consumeOrFailWithFlags(COLON, TreeBuilder::DontBuildStrings, "Expected ':' in ternary operator");
    
    TreeExpression rhs = parseAssignmentExpression(context);
    failIfFalse(rhs, "Cannot parse right hand side of ternary operator");
    context.setEndOffset(rhs, m_lastTokenEndPosition.offset);
    return context.createConditionalExpr(location, cond, lhs, rhs);
}

ALWAYS_INLINE static bool isUnaryOpExcludingUpdateOp(JSTokenType token)
{
    if (isUpdateOp(token))
        return false;
    return isUnaryOp(token);
}

template <typename LexerType>
int Parser<LexerType>::isBinaryOperator(JSTokenType token)
{
    if (m_allowsIn)
        return token & (BinaryOpTokenPrecedenceMask << BinaryOpTokenAllowsInPrecedenceAdditionalShift);
    return token & BinaryOpTokenPrecedenceMask;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseBinaryExpression(TreeBuilder& context)
{
    int operandStackDepth = 0;
    int operatorStackDepth = 0;
    typename TreeBuilder::BinaryExprContext binaryExprContext(context);
    JSTokenLocation location(tokenLocation());
    bool hasLogicalOperator = false;
    bool hasCoalesceOperator = false;

    int previousOperator = 0;
    while (true) {
        JSTextPosition exprStart = tokenStartPosition();
        int initialAssignments = m_parserState.assignmentCount;
        JSTokenType leadingTokenTypeForUnaryExpression = m_token.m_type;

        TreeExpression current = 0;
        if (match(PRIVATENAME)) {
            const Identifier* ident = m_token.m_data.ident;
            ASSERT(ident);
            currentScope()->usePrivateName(*ident);
            m_seenPrivateNameUseInNonReparsingFunctionMode = true;
            next();
            semanticFailIfTrue(m_token.m_type != INTOKEN || previousOperator >= INTOKEN, "Bare private name can only be used as the left-hand side of an `in` expression");
            current = context.createPrivateIdentifierNode(location, *ident);
        } else
            current = parseUnaryExpression(context);
        failIfFalse(current, "Cannot parse expression");

        context.appendBinaryExpressionInfo(operandStackDepth, current, exprStart, lastTokenEndPosition(), lastTokenEndPosition(), initialAssignments != m_parserState.assignmentCount);
        int precedence = isBinaryOperator(m_token.m_type);
        if (!precedence)
            break;

        // 12.6 https://tc39.github.io/ecma262/#sec-exp-operator
        // ExponentiationExpresion is described as follows.
        //
        //     ExponentiationExpression[Yield]:
        //         UnaryExpression[?Yield]
        //         UpdateExpression[?Yield] ** ExponentiationExpression[?Yield]
        //
        // As we can see, the left hand side of the ExponentiationExpression is UpdateExpression, not UnaryExpression.
        // So placing UnaryExpression not included in UpdateExpression here is a syntax error.
        // This is intentional. For example, if UnaryExpression is allowed, we can have the code like `-x**y`.
        // But this is confusing: `-(x**y)` OR `(-x)**y`, which interpretation is correct?
        // To avoid this problem, ECMA262 makes unparenthesized exponentiation expression as operand of unary operators an early error.
        // More rationale: https://mail.mozilla.org/pipermail/es-discuss/2015-September/044232.html
        //
        // Here, we guarantee that the left hand side of this expression is not unary expression by checking the leading operator of the parseUnaryExpression.
        // This check just works. Let's consider the example,
        //     y <> -x ** z
        //          ^
        //          Check this.
        // If the binary operator <> has higher precedence than one of "**", this check does not work.
        // But it's OK for ** because the operator "**" has the highest operator precedence in the binary operators.
        failIfTrue(match(POW) && isUnaryOpExcludingUpdateOp(leadingTokenTypeForUnaryExpression), "Ambiguous unary expression in the left hand side of the exponentiation expression; parentheses must be used to disambiguate the expression");

        // Mixing ?? with || or && is currently specified as an early error.
        // Since ?? is the lowest-precedence binary operator, it suffices to check whether these ever coexist in the operator stack.
        if (match(AND) || match(OR))
            hasLogicalOperator = true;
        else if (match(COALESCE))
            hasCoalesceOperator = true;
        failIfTrue(hasLogicalOperator && hasCoalesceOperator, "Coalescing and logical operators used together in the same expression; parentheses must be used to disambiguate");

        m_parserState.nonTrivialExpressionCount++;
        m_parserState.nonLHSCount++;
        int operatorToken = m_token.m_type;
        next(TreeBuilder::DontBuildStrings);
        
        while (operatorStackDepth && context.operatorStackShouldReduce(precedence)) {
            ASSERT(operandStackDepth > 1);
            
            typename TreeBuilder::BinaryOperand rhs = context.getFromOperandStack(-1);
            typename TreeBuilder::BinaryOperand lhs = context.getFromOperandStack(-2);
            context.shrinkOperandStackBy(operandStackDepth, 2);
            context.appendBinaryOperation(location, operandStackDepth, operatorStackDepth, lhs, rhs);
            context.operatorStackPop(operatorStackDepth);
        }
        context.operatorStackAppend(operatorStackDepth, operatorToken, precedence);
        previousOperator = operatorToken;
    }
    while (operatorStackDepth) {
        ASSERT(operandStackDepth > 1);
        
        typename TreeBuilder::BinaryOperand rhs = context.getFromOperandStack(-1);
        typename TreeBuilder::BinaryOperand lhs = context.getFromOperandStack(-2);
        context.shrinkOperandStackBy(operandStackDepth, 2);
        context.appendBinaryOperation(location, operandStackDepth, operatorStackDepth, lhs, rhs);
        context.operatorStackPop(operatorStackDepth);
    }
    return context.popOperandStack(operandStackDepth);
}

template <typename LexerType>
template <class TreeBuilder> TreeProperty Parser<LexerType>::parseProperty(TreeBuilder& context)
{
    SourceParseMode parseMode = SourceParseMode::MethodMode;
    bool wasIdent = false;
    std::optional<unsigned> timesPosition;
    std::optional<unsigned> asyncPosition;

    if (match(TIMES)) {
        timesPosition = m_token.m_startPosition;
        next();
        parseMode = SourceParseMode::GeneratorWrapperMethodMode;
    }

parseProperty:
    switch (m_token.m_type) {
    case ESCAPED_KEYWORD:
    case IDENT:
        if (*m_token.m_data.ident == m_vm.propertyNames->async && !m_token.m_data.escaped) [[unlikely]] {
            asyncPosition = m_token.m_startPosition;
            if (parseMode == SourceParseMode::MethodMode) {
                SavePoint savePoint = createSavePoint(context);
                next();

                if (match(COLON) || match(OPENPAREN) || match(COMMA) || match(CLOSEBRACE)) {
                    restoreSavePoint(context, savePoint);
                    wasIdent = true;
                    goto namedProperty;
                }

                failIfTrue(m_lexer->hasLineTerminatorBeforeToken(), "Expected a property name following keyword 'async'");
                if (consume(TIMES)) [[unlikely]]
                    parseMode = SourceParseMode::AsyncGeneratorWrapperMethodMode;
                else
                    parseMode = SourceParseMode::AsyncMethodMode;
                goto parseProperty;
            }
        }
        [[fallthrough]];
    case YIELD:
    case AWAIT:
        wasIdent = true;
        [[fallthrough]];
    case STRING: {
namedProperty:
        const Identifier* ident = m_token.m_data.ident;
        bool wasUnescapedIdent = wasIdent && !m_token.m_data.escaped;
        unsigned getterOrSetterStartOffset = tokenStart();
        unsigned functionStart = timesPosition.value_or(asyncPosition.value_or(m_token.m_startPosition));
        JSToken identToken = m_token;

        if (wasUnescapedIdent && !isGeneratorMethodParseMode(parseMode) && (*ident == m_vm.propertyNames->get || *ident == m_vm.propertyNames->set))
            nextExpectIdentifier(LexerFlags::IgnoreReservedWords);
        else
            nextExpectIdentifier(TreeBuilder::DontBuildKeywords | LexerFlags::IgnoreReservedWords);

        if (!isGeneratorMethodParseMode(parseMode) && !isAsyncMethodParseMode(parseMode) && match(COLON)) {
            next();
            TreeExpression node = parseAssignmentExpressionOrPropagateErrorClass(context);
            failIfFalse(node, "Cannot parse expression for property declaration");
            context.setEndOffset(node, m_lexer->currentOffset());
            InferName inferName = ident && *ident == m_vm.propertyNames->underscoreProto ? InferName::Disallowed : InferName::Allowed;
            return context.createProperty(ident, node, PropertyNode::Constant, SuperBinding::NotNeeded, inferName, ClassElementTag::No);
        }

        if (match(OPENPAREN)) {
            SetForScope innerParseMode(m_parseMode, parseMode);
            auto method = parsePropertyMethod(context, ident, functionStart);
            propagateError();
            return context.createProperty(ident, method, PropertyNode::Constant, SuperBinding::Needed, InferName::Allowed, ClassElementTag::No);
        }
        failIfTrue(parseMode != SourceParseMode::MethodMode, "Expected a parenthesis for argument list");

        failIfFalse(wasIdent, "Expected an identifier as property name");

        if (match(COMMA) || match(CLOSEBRACE)) {
            semanticFailureDueToKeywordCheckingToken(identToken, "shorthand property name");
            JSTextPosition start = tokenStartPosition();
            JSTokenLocation location(tokenLocation());
            currentScope()->useVariable(ident, m_vm.propertyNames->eval == *ident);
            if (currentScope()->isArrowFunction())
                currentScope()->setInnerArrowFunctionUsesEval();
            TreeExpression node = context.createResolve(location, *ident, start, lastTokenEndPosition());
            return context.createProperty(ident, node, static_cast<PropertyNode::Type>(PropertyNode::Constant | PropertyNode::Shorthand), SuperBinding::NotNeeded, InferName::Allowed, ClassElementTag::No);
        }

        if (match(EQUAL)) // CoverInitializedName is exclusive to BindingPattern and AssignmentPattern
            classifyExpressionError(ErrorIndicatesPattern);

        std::optional<PropertyNode::Type> type;
        if (wasUnescapedIdent) {
            if (*ident == m_vm.propertyNames->get)
                type = PropertyNode::Getter;
            else if (*ident == m_vm.propertyNames->set)
                type = PropertyNode::Setter;
        }
        failIfFalse(type, "Expected a ':' following the property name '", ident->impl(), "'");
        return parseGetterSetter(context, type.value(), getterOrSetterStartOffset, ConstructorKind::None, ClassElementTag::No);
    }
    case DOUBLE:
    case INTEGER: {
        unsigned functionStart = timesPosition.value_or(asyncPosition.value_or(m_token.m_startPosition));
        const Identifier& ident = m_parserArena.identifierArena().makeNumericIdentifier(const_cast<VM&>(m_vm), m_token.m_data.doubleValue);
        next();

        if (match(OPENPAREN)) {
            SetForScope innerParseMode(m_parseMode, parseMode);
            auto method = parsePropertyMethod(context, &ident, functionStart);
            propagateError();
            return context.createProperty(&ident, method, PropertyNode::Constant, SuperBinding::Needed, InferName::Allowed, ClassElementTag::No);
        }
        failIfTrue(parseMode != SourceParseMode::MethodMode, "Expected a parenthesis for argument list");

        consumeOrFail(COLON, "Expected ':' after property name");
        TreeExpression node = parseAssignmentExpression(context);
        failIfFalse(node, "Cannot parse expression for property declaration");
        context.setEndOffset(node, m_lexer->currentOffset());
        return context.createProperty(&ident, node, PropertyNode::Constant, SuperBinding::NotNeeded, InferName::Allowed, ClassElementTag::No);
    }
    case BIGINT: {
        const Identifier* ident = m_parserArena.identifierArena().makeBigIntDecimalIdentifier(const_cast<VM&>(m_vm), *m_token.m_data.bigIntString, m_token.m_data.radix);
        failIfFalse(ident, "Cannot parse big int property name");
        unsigned functionStart = timesPosition.value_or(asyncPosition.value_or(m_token.m_startPosition));
        next();

        if (match(OPENPAREN)) {
            SetForScope innerParseMode(m_parseMode, parseMode);
            auto method = parsePropertyMethod(context, ident, functionStart);
            propagateError();
            return context.createProperty(ident, method, PropertyNode::Constant, SuperBinding::Needed, InferName::Allowed, ClassElementTag::No);
        }
        failIfTrue(parseMode != SourceParseMode::MethodMode, "Expected a parenthesis for argument list");

        consumeOrFail(COLON, "Expected ':' after property name");
        TreeExpression node = parseAssignmentExpression(context);
        failIfFalse(node, "Cannot parse expression for property declaration");
        context.setEndOffset(node, m_lexer->currentOffset());
        return context.createProperty(ident, node, PropertyNode::Constant, SuperBinding::NotNeeded, InferName::Allowed, ClassElementTag::No);
    }
    case OPENBRACKET: {
        unsigned functionStart = timesPosition.value_or(asyncPosition.value_or(m_token.m_startPosition));
        next();
        auto propertyName = parseAssignmentExpression(context);
        failIfFalse(propertyName, "Cannot parse computed property name");
        handleProductionOrFail(CLOSEBRACKET, "]", "end", "computed property name");

        if (match(OPENPAREN)) {
            SetForScope innerParseMode(m_parseMode, parseMode);
            auto method = parsePropertyMethod(context, &m_vm.propertyNames->nullIdentifier, functionStart);
            propagateError();
            return context.createProperty(propertyName, method, static_cast<PropertyNode::Type>(PropertyNode::Constant | PropertyNode::Computed), SuperBinding::Needed, ClassElementTag::No);
        }
        failIfTrue(parseMode != SourceParseMode::MethodMode, "Expected a parenthesis for argument list");

        consumeOrFail(COLON, "Expected ':' after property name");
        TreeExpression node = parseAssignmentExpression(context);
        failIfFalse(node, "Cannot parse expression for property declaration");
        context.setEndOffset(node, m_lexer->currentOffset());
        return context.createProperty(propertyName, node, static_cast<PropertyNode::Type>(PropertyNode::Constant | PropertyNode::Computed), SuperBinding::NotNeeded, ClassElementTag::No);
    }
    case DOTDOTDOT: {
        auto spreadLocation = m_token.m_location;
        auto start = m_token.m_startPosition;
        auto divot = m_token.m_endPosition;
        next();
        TreeExpression elem = parseAssignmentExpressionOrPropagateErrorClass(context);
        failIfFalse(elem, "Cannot parse subject of a spread operation");
        auto node = context.createObjectSpreadExpression(spreadLocation, elem, start, divot, m_lastTokenEndPosition);
        return context.createProperty(node, PropertyNode::Spread, SuperBinding::NotNeeded, ClassElementTag::No);
    }
    default:
        failIfFalse(m_token.m_type & KeywordTokenFlag, "Expected a property name");
        wasIdent = true; // Treat keyword token as an identifier
        goto namedProperty;
    }
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parsePropertyMethod(TreeBuilder& context, const Identifier* methodName, unsigned functionStart)
{
    ASSERT(isMethodParseMode(sourceParseMode()));
    JSTokenLocation methodLocation(tokenLocation());
    ParserFunctionInfo<TreeBuilder> methodInfo;
    methodInfo.name = methodName;
    failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::Unnamed, false, ConstructorKind::None, SuperBinding::Needed, functionStart, methodInfo, FunctionDefinitionType::Method)), "Cannot parse this method");
    return context.createMethodDefinition(methodLocation, methodInfo);
}

template <typename LexerType>
template <class TreeBuilder> TreeProperty Parser<LexerType>::parseGetterSetter(TreeBuilder& context, PropertyNode::Type type, unsigned getterOrSetterStartOffset,
    ConstructorKind constructorKind, ClassElementTag tag)
{
    const Identifier* stringPropertyName = nullptr;
    double numericPropertyName = 0;
    TreeExpression computedPropertyName = 0;

    JSTokenLocation location(tokenLocation());

    bool matchesPrivateName = match(PRIVATENAME);
    if (matchSpecIdentifier() || match(STRING) || matchesPrivateName || m_token.m_type & KeywordTokenFlag) {
        stringPropertyName = m_token.m_data.ident;
        semanticFailIfTrue(tag == ClassElementTag::Static && *stringPropertyName == m_vm.propertyNames->prototype,
            "Cannot declare a static method named 'prototype'");
        semanticFailIfTrue(tag == ClassElementTag::Instance && *stringPropertyName == m_vm.propertyNames->constructor,
            "Cannot declare a getter or setter named 'constructor'");
        semanticFailIfTrue(*stringPropertyName == m_vm.propertyNames->constructorPrivateField, "Cannot declare a private accessor named '#constructor'");

        if (match(PRIVATENAME))
            semanticFailIfTrue(tag == ClassElementTag::No, "Cannot declare a private setter or getter outside a class");
        next();
    } else if (match(DOUBLE) || match(INTEGER)) {
        numericPropertyName = m_token.m_data.doubleValue;
        next();
    } else if (match(BIGINT)) {
        stringPropertyName = m_parserArena.identifierArena().makeBigIntDecimalIdentifier(const_cast<VM&>(m_vm), *m_token.m_data.bigIntString, m_token.m_data.radix);
        failIfFalse(stringPropertyName, "Cannot parse big int property name");
        next();
    } else if (consume(OPENBRACKET)) [[likely]] {
        computedPropertyName = parseAssignmentExpression(context);
        failIfFalse(computedPropertyName, "Cannot parse computed property name");
        handleProductionOrFail(CLOSEBRACKET, "]", "end", "computed property name");
    } else
        failDueToUnexpectedToken();

    ParserFunctionInfo<TreeBuilder> info;
    if (type & PropertyNode::Getter) {
        failIfFalse(match(OPENPAREN), "Expected a parameter list for getter definition");
        SetForScope innerParseMode(m_parseMode, SourceParseMode::GetterMode);
        failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::Unnamed, false, constructorKind, SuperBinding::Needed, getterOrSetterStartOffset, info, FunctionDefinitionType::Method)), "Cannot parse getter definition");
    } else if (type & PropertyNode::Setter) {
        failIfFalse(match(OPENPAREN), "Expected a parameter list for setter definition");
        SetForScope innerParseMode(m_parseMode, SourceParseMode::SetterMode);
        failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::Unnamed, false, constructorKind, SuperBinding::Needed, getterOrSetterStartOffset, info, FunctionDefinitionType::Method)), "Cannot parse setter definition");
    } else if (type & PropertyNode::PrivateSetter) {
        failIfFalse(match(OPENPAREN), "Expected a parameter list for private setter definition");
        SetForScope innerParseMode(m_parseMode, SourceParseMode::SetterMode);
        failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::Unnamed, false, constructorKind, SuperBinding::Needed, getterOrSetterStartOffset, info, FunctionDefinitionType::Method)), "Cannot parse private setter definition");
    } else if (type & PropertyNode::PrivateGetter) {
        failIfFalse(match(OPENPAREN), "Expected a parameter list for private getter definition");
        SetForScope innerParseMode(m_parseMode, SourceParseMode::GetterMode);
        failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::Unnamed, false, constructorKind, SuperBinding::Needed, getterOrSetterStartOffset, info, FunctionDefinitionType::Method)), "Cannot parse private getter definition");
    }

    if (stringPropertyName)
        return context.createGetterOrSetterProperty(location, type, stringPropertyName, info, tag);

    if (computedPropertyName)
        return context.createGetterOrSetterProperty(location, static_cast<PropertyNode::Type>(type | PropertyNode::Computed), computedPropertyName, info, tag);

    return context.createGetterOrSetterProperty(const_cast<VM&>(m_vm), m_parserArena, location, type, numericPropertyName, info, tag);
}

template <typename LexerType>
void Parser<LexerType>::recordPauseLocation(const JSTextPosition& position)
{
    if (!m_debuggerParseData) [[likely]]
        return;

    if (position.line < 0)
        return;

    m_debuggerParseData->pausePositions.appendPause(position);
}

template <typename LexerType>
void Parser<LexerType>::recordFunctionEntryLocation(const JSTextPosition& position)
{
    if (!m_debuggerParseData) [[likely]]
        return;

    m_debuggerParseData->pausePositions.appendEntry(position);
}

template <typename LexerType>
void Parser<LexerType>::recordFunctionLeaveLocation(const JSTextPosition& position)
{
    if (!m_debuggerParseData) [[likely]]
        return;

    m_debuggerParseData->pausePositions.appendLeave(position);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseObjectLiteral(TreeBuilder& context)
{
    JSTokenLocation location(tokenLocation());
    consumeOrFail(OPENBRACE, "Expected opening '{' at the start of an object literal");

    SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
    if (consume(CLOSEBRACE))
        return context.createObjectLiteral(location);
    
    TreeProperty property = parseProperty(context);
    failIfFalse(property, "Cannot parse object literal property");

    bool seenProtoSetter = context.isUnderscoreProtoSetter(property);

    TreePropertyList propertyList = context.createPropertyList(location, property);
    TreePropertyList tail = propertyList;
    while (consume(COMMA)) {
        if (match(CLOSEBRACE))
            break;
        JSTokenLocation propertyLocation(tokenLocation());
        property = parseProperty(context);
        failIfFalse(property, "Cannot parse object literal property");
        if (context.isUnderscoreProtoSetter(property)) {
            // https://tc39.es/ecma262/#sec-__proto__-property-names-in-object-initializers
            semanticFailIfTrue(seenProtoSetter, "Attempted to redefine __proto__ property");
            seenProtoSetter = true;
        }
        tail = context.createPropertyList(propertyLocation, property, tail);
    }

    handleProductionOrFail2(CLOSEBRACE, "}", "end", "object literal");

    return context.createObjectLiteral(location, propertyList);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseArrayLiteral(TreeBuilder& context)
{
    JSTokenLocation location(tokenLocation());
    consumeOrFailWithFlags(OPENBRACKET, TreeBuilder::DontBuildStrings, "Expected an opening '[' at the beginning of an array literal");

    SetForScope nonLHSCountScope(m_parserState.nonLHSCount);

    int elisions = 0;
    while (match(COMMA)) {
        next(TreeBuilder::DontBuildStrings);
        elisions++;
    }
    if (consume(CLOSEBRACKET))
        return context.createArray(location, elisions);
    
    TreeExpression elem;
    if (match(DOTDOTDOT)) [[unlikely]] {
        auto spreadLocation = m_token.m_location;
        auto start = m_token.m_startPosition;
        auto divot = m_token.m_endPosition;
        next();
        auto spreadExpr = parseAssignmentExpressionOrPropagateErrorClass(context);
        failIfFalse(spreadExpr, "Cannot parse subject of a spread operation");
        elem = context.createSpreadExpression(spreadLocation, spreadExpr, start, divot, m_lastTokenEndPosition);
    } else
        elem = parseAssignmentExpressionOrPropagateErrorClass(context);
    failIfFalse(elem, "Cannot parse array literal element");
    typename TreeBuilder::ElementList elementList = context.createElementList(elisions, elem);
    typename TreeBuilder::ElementList tail = elementList;
    elisions = 0;
    while (match(COMMA)) {
        next(TreeBuilder::DontBuildStrings);
        elisions = 0;
        
        while (consume(COMMA))
            elisions++;
        
        if (consume(CLOSEBRACKET))
            return context.createArray(location, elisions, elementList);

        if (match(DOTDOTDOT)) [[unlikely]] {
            auto spreadLocation = m_token.m_location;
            auto start = m_token.m_startPosition;
            auto divot = m_token.m_endPosition;
            next();
            TreeExpression elem = parseAssignmentExpressionOrPropagateErrorClass(context);
            failIfFalse(elem, "Cannot parse subject of a spread operation");
            auto spread = context.createSpreadExpression(spreadLocation, elem, start, divot, m_lastTokenEndPosition);
            tail = context.createElementList(tail, elisions, spread);
            continue;
        }
        TreeExpression elem = parseAssignmentExpressionOrPropagateErrorClass(context);
        failIfFalse(elem, "Cannot parse array literal element");
        tail = context.createElementList(tail, elisions, elem);
    }

    if (!consume(CLOSEBRACKET)) [[unlikely]] {
        failIfFalse(match(DOTDOTDOT), "Expected either a closing ']' or a ',' following an array element");
        semanticFail("The '...' operator should come before a target expression");
    }

    return context.createArray(location, elementList);
}

template <typename LexerType>
template <class TreeBuilder> TreeClassExpression Parser<LexerType>::parseClassExpression(TreeBuilder& context)
{
    ASSERT(match(CLASSTOKEN));
    SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
    ParserClassInfo<TreeBuilder> info;
    info.className = &m_vm.propertyNames->nullIdentifier;
    return parseClass(context, FunctionNameRequirements::None, info);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseFunctionExpression(TreeBuilder& context)
{
    ASSERT(match(FUNCTION));
    SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
    JSTokenLocation location(tokenLocation());
    unsigned functionStart = tokenStart();
    next();
    ParserFunctionInfo<TreeBuilder> functionInfo;
    functionInfo.name = &m_vm.propertyNames->nullIdentifier;
    SourceParseMode parseMode = SourceParseMode::NormalFunctionMode;
    if (consume(TIMES))
        parseMode = SourceParseMode::GeneratorWrapperFunctionMode;
    SetForScope setInnerParseMode(m_parseMode, parseMode);

    ConstructorKind constructorKind = currentScope()->isGlobalCode() ? m_constructorKindForTopLevelFunctionExpressions : ConstructorKind::None;
    SuperBinding expectedSuperBinding = constructorKind == ConstructorKind::Extends ? SuperBinding::Needed : SuperBinding::NotNeeded;

    failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::None, false, constructorKind, expectedSuperBinding, functionStart, functionInfo, FunctionDefinitionType::Expression)), "Cannot parse function expression");
    return context.createFunctionExpr(location, functionInfo);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseAsyncFunctionExpression(TreeBuilder& context, const JSTokenLocation& location)
{
    ASSERT(match(FUNCTION));
    next();
    SourceParseMode parseMode = SourceParseMode::AsyncFunctionMode;

    if (consume(TIMES))
        parseMode = SourceParseMode::AsyncGeneratorWrapperFunctionMode;
    SetForScope setInnerParseMode(m_parseMode, parseMode);

    ParserFunctionInfo<TreeBuilder> functionInfo;
    functionInfo.name = &m_vm.propertyNames->nullIdentifier;
    failIfFalse(parseFunctionInfo(context, FunctionNameRequirements::None, false, ConstructorKind::None, SuperBinding::NotNeeded, location.startOffset, functionInfo, FunctionDefinitionType::Expression), parseMode == SourceParseMode::AsyncFunctionMode ? "Cannot parse async function expression" : "Cannot parse async generator function expression");
    return context.createFunctionExpr(location, functionInfo);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::TemplateString Parser<LexerType>::parseTemplateString(TreeBuilder& context, bool isTemplateHead, typename LexerType::RawStringsBuildMode rawStringsBuildMode, bool& elementIsTail)
{
    if (isTemplateHead)
        ASSERT(match(BACKQUOTE));
    else
        matchOrFail(CLOSEBRACE, "Expected a closing '}' following an expression in template literal");

    // Re-scan the token to recognize it as Template Element.
    m_token.m_type = m_lexer->scanTemplateString(&m_token, rawStringsBuildMode);
    matchOrFail(TEMPLATE, "Expected an template element");
    const Identifier* cooked = m_token.m_data.cooked;
    const Identifier* raw = m_token.m_data.raw;
    elementIsTail = m_token.m_data.isTail;
    JSTokenLocation location(tokenLocation());
    next();
    return context.createTemplateString(location, cooked, raw);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::TemplateLiteral Parser<LexerType>::parseTemplateLiteral(TreeBuilder& context, typename LexerType::RawStringsBuildMode rawStringsBuildMode)
{
    ASSERT(match(BACKQUOTE));
    SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
    JSTokenLocation location(tokenLocation());
    bool elementIsTail = false;

    auto headTemplateString = parseTemplateString(context, true, rawStringsBuildMode, elementIsTail);
    failIfFalse(headTemplateString, "Cannot parse head template element");

    typename TreeBuilder::TemplateStringList templateStringList = context.createTemplateStringList(headTemplateString);
    typename TreeBuilder::TemplateStringList templateStringTail = templateStringList;

    if (elementIsTail)
        return context.createTemplateLiteral(location, templateStringList);

    failIfTrue(match(CLOSEBRACE), "Template literal expression cannot be empty");
    TreeExpression expression = parseExpression(context);
    failIfFalse(expression, "Cannot parse expression in template literal");

    typename TreeBuilder::TemplateExpressionList templateExpressionList = context.createTemplateExpressionList(expression);
    typename TreeBuilder::TemplateExpressionList templateExpressionTail = templateExpressionList;

    auto templateString = parseTemplateString(context, false, rawStringsBuildMode, elementIsTail);
    failIfFalse(templateString, "Cannot parse template element");
    templateStringTail = context.createTemplateStringList(templateStringTail, templateString);

    while (!elementIsTail) {
        failIfTrue(match(CLOSEBRACE), "Template literal expression cannot be empty");
        TreeExpression expression = parseExpression(context);
        failIfFalse(expression, "Cannot parse expression in template literal");

        templateExpressionTail = context.createTemplateExpressionList(templateExpressionTail, expression);

        auto templateString = parseTemplateString(context, false, rawStringsBuildMode, elementIsTail);
        failIfFalse(templateString, "Cannot parse template element");
        templateStringTail = context.createTemplateStringList(templateStringTail, templateString);
    }

    return context.createTemplateLiteral(location, templateStringList, templateExpressionList);
}

template <class LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::createResolveAndUseVariable(TreeBuilder& context, const Identifier* ident, bool isEval, const JSTextPosition& start, const JSTokenLocation& location)
{
    currentScope()->useVariable(ident, isEval);
    m_parserState.lastIdentifier = ident;
    return context.createResolve(location, *ident, start, lastTokenEndPosition());
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::tryParseArgumentsDotLengthForFastPath(TreeBuilder& context)
{
    // There is a fast path for getting `arguments.length` by reading `argumentCountIncludingThis`
    // directly from CallFrame. In that case, no need to materialize `arguments` object. The fast
    // path for `arguments.length` is applied by excluding 'arguments.length` pattern for
    // ArgumentsFeature except for two cases:
    // 1. 'arguments.length` modifications.
    // 2. Function level global variable declaration with identifier `arguments`.
    if (context.hasArgumentsFeature() || !match(IDENT) || !isArgumentsIdentifier())
        return 0;

    // If semantic checks fail here, then let `parsePrimaryExpression` handle the error thrown.
    // Note that these checks must align to the checks in `parsePrimaryExpression` under
    // the clause with token type IDENT.
    if (currentScope()->isStaticBlock()
        || m_parserState.isParsingClassFieldInitializer
        || currentScope()->evalContextType() == EvalContextType::InstanceFieldEvalContext) [[unlikely]]
        return 0;

    SavePoint argumentsSavePoint = createSavePoint(context);
    JSTextPosition primaryStart = tokenStartPosition();
    JSTokenLocation primaryLocation(tokenLocation());
    next();
    if (match(DOT)) {
        SavePoint argumentsDotSavePoint = createSavePoint(context);
        next();
        if (match(IDENT) && *m_token.m_data.ident == m_vm.propertyNames->length) {
            m_seenArgumentsDotLength = true;

            bool isEval = false;
            const Identifier* argumentsIdentifier = &m_vm.propertyNames->arguments;
            currentScope()->useVariable(argumentsIdentifier, isEval);
            m_parserState.lastIdentifier = argumentsIdentifier;

            bool needToCheckUsesArguments = false;
            TreeExpression argumentsDotExpression = context.createResolve(primaryLocation, *argumentsIdentifier, primaryStart, lastTokenEndPosition(), needToCheckUsesArguments);
            restoreSavePoint(context, argumentsDotSavePoint);
            return argumentsDotExpression;
        }
    }
    restoreSavePoint(context, argumentsSavePoint);
    return 0;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parsePrimaryExpression(TreeBuilder& context)
{
    failIfStackOverflow();
    switch (m_token.m_type) {
    case FUNCTION:
        return parseFunctionExpression(context);
    case CLASSTOKEN:
        return parseClassExpression(context);
    case OPENBRACE:
        return parseObjectLiteral(context);
    case OPENBRACKET:
        return parseArrayLiteral(context);
    case OPENPAREN: {
        next();
        SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
        TreeExpression result = parseExpression(context);
        handleProductionOrFail(CLOSEPAREN, ")", "end", "compound expression");
        return result;
    }
    case THISTOKEN: {
        JSTokenLocation location(tokenLocation());
        next();
        if (currentScope()->isArrowFunction())
            currentScope()->setInnerArrowFunctionUsesThis();
        return context.createThisExpr(location);
    }
    case AWAIT:
        semanticFailIfTrue(currentScope()->isStaticBlock(), "The 'await' keyword is disallowed in the IdentifierReference position within static block");
        if (m_parserState.functionParsePhase == FunctionParsePhase::Parameters)
            semanticFailIfFalse(m_parserState.allowAwait, "Cannot use 'await' within a parameter default expression");
        else if (!m_parserState.classFieldInitMasksAsync && (currentFunctionScope()->isAsyncFunctionBoundary() || isModuleParseMode(sourceParseMode())))
            return parseAwaitExpression(context);

        goto identifierExpression;
    case IDENT: {
        semanticFailIfTrue(currentScope()->isStaticBlock() && isArgumentsIdentifier(), "Cannot use 'arguments' as an identifier in static block");
        if (*m_token.m_data.ident == m_vm.propertyNames->async && !m_token.m_data.escaped) [[unlikely]] {
            JSTextPosition functionStart = tokenStartPosition();
            const Identifier* ident = m_token.m_data.ident;
            JSTokenLocation location(tokenLocation());
            next();
            if (match(FUNCTION) && !m_lexer->hasLineTerminatorBeforeToken())
                return parseAsyncFunctionExpression(context, location);

            // Avoid using variable if it is an arrow function parameter
            if (match(ARROWFUNCTION)) [[unlikely]]
                return 0;

            const bool isEval = false;
            return createResolveAndUseVariable(context, ident, isEval, functionStart, location);
        }
        if (m_parserState.isParsingClassFieldInitializer) [[unlikely]]
            failIfTrue(isArgumentsIdentifier(), "Cannot reference 'arguments' in class field initializer");
    identifierExpression:
        JSTextPosition start = tokenStartPosition();
        const Identifier* ident = m_token.m_data.ident;
        if (currentScope()->evalContextType() == EvalContextType::InstanceFieldEvalContext) [[unlikely]]
            failIfTrue(*ident == m_vm.propertyNames->arguments, "arguments is not valid in this context");
        JSTokenLocation location(tokenLocation());
        next();

        // Avoid using variable if it is an arrow function parameter
        if (match(ARROWFUNCTION)) [[unlikely]]
            return 0;

        return createResolveAndUseVariable(context, ident, *ident == m_vm.propertyNames->eval, start, location);
    }
    case BIGINT: {
        const Identifier* ident = m_token.m_data.bigIntString;
        uint8_t radix = m_token.m_data.radix;
        JSTokenLocation location(tokenLocation());
        next();
        return context.createBigInt(location, ident, radix);
    }
    case STRING: {
        const Identifier* ident = m_token.m_data.ident;
        JSTokenLocation location(tokenLocation());
        next();
        return context.createString(location, ident);
    }
    case DOUBLE: {
        double d = m_token.m_data.doubleValue;
        JSTokenLocation location(tokenLocation());
        next();
        return context.createDoubleExpr(location, d);
    }
    case INTEGER: {
        double d = m_token.m_data.doubleValue;
        JSTokenLocation location(tokenLocation());
        next();
        return context.createIntegerExpr(location, d);
    }
    case NULLTOKEN: {
        JSTokenLocation location(tokenLocation());
        next();
        return context.createNull(location);
    }
    case TRUETOKEN: {
        JSTokenLocation location(tokenLocation());
        next();
        return context.createBoolean(location, true);
    }
    case FALSETOKEN: {
        JSTokenLocation location(tokenLocation());
        next();
        return context.createBoolean(location, false);
    }
    case DIVEQUAL:
    case DIVIDE: {
        /* regexp */
        if (match(DIVEQUAL))
            m_token.m_type = m_lexer->scanRegExp(&m_token, '=');
        else
            m_token.m_type = m_lexer->scanRegExp(&m_token);
        matchOrFail(REGEXP, "Invalid regular expression");

        const Identifier* pattern = m_token.m_data.pattern;
        const Identifier* flags = m_token.m_data.flags;
        JSTextPosition start = tokenStartPosition();
        JSTokenLocation location(tokenLocation());
        next();
        TreeExpression re = context.createRegExp(location, *pattern, *flags, start);
        if (!re) [[unlikely]] {
            Yarr::ErrorCode errorCode = Yarr::checkSyntax(pattern->string(), flags->string());
            regexFail(String::fromLatin1(Yarr::errorMessage(errorCode)));
        }
        return re;
    }
    case BACKQUOTE:
        return parseTemplateLiteral(context, LexerType::RawStringsBuildMode::DontBuildRawStrings);
    case YIELD:
        if (canUseIdentifierYield()) [[likely]]
            goto identifierExpression;
        failDueToUnexpectedToken();
    case LET:
        if (!strictMode()) [[likely]]
            goto identifierExpression;
        failDueToUnexpectedToken();
    case ESCAPED_KEYWORD:
        if (matchAllowedEscapedContextualKeyword()) [[likely]]
            goto identifierExpression;
        [[fallthrough]];
    default:
        failDueToUnexpectedToken();
    }
}

template <typename LexerType>
template <class TreeBuilder> TreeArguments Parser<LexerType>::parseArguments(TreeBuilder& context)
{
    consumeOrFailWithFlags(OPENPAREN, TreeBuilder::DontBuildStrings, "Expected opening '(' at start of argument list");
    JSTokenLocation location(tokenLocation());
    if (match(CLOSEPAREN)) {
        next();
        return context.createArguments();
    }
    auto argumentsStart = m_token.m_startPosition;
    auto argumentsDivot = m_token.m_endPosition;

    int initialAssignments = m_parserState.assignmentCount;
    ArgumentType argType = ArgumentType::Normal;
    TreeExpression firstArg = parseArgument(context, argType);
    failIfFalse(firstArg, "Cannot parse function argument");
    semanticFailIfTrue(match(DOTDOTDOT), "The '...' operator should come before the target expression");

    bool hasSpread = false;
    if (argType == ArgumentType::Spread)
        hasSpread = true;
    TreeArgumentsList argList = context.createArgumentsList(location, firstArg);
    TreeArgumentsList tail = argList;

    while (match(COMMA)) {
        JSTokenLocation argumentLocation(tokenLocation());
        next(TreeBuilder::DontBuildStrings);

        if (match(CLOSEPAREN)) [[unlikely]]
            break;
        
        TreeExpression arg = parseArgument(context, argType);
        propagateError();
        semanticFailIfTrue(match(DOTDOTDOT), "The '...' operator should come before the target expression");

        if (argType == ArgumentType::Spread)
            hasSpread = true;

        tail = context.createArgumentsList(argumentLocation, tail, arg);
    }

    handleProductionOrFail2(CLOSEPAREN, ")", "end", "argument list");
    if (hasSpread) {
        TreeExpression spreadArray = context.createSpreadExpression(location, context.createArray(location, context.createElementList(argList)), argumentsStart, argumentsDivot, m_lastTokenEndPosition);
        return context.createArguments(context.createArgumentsList(location, spreadArray), initialAssignments != m_parserState.assignmentCount);
    }

    return context.createArguments(argList, initialAssignments != m_parserState.assignmentCount);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseArgument(TreeBuilder& context, ArgumentType& type)
{
    if (match(DOTDOTDOT)) [[unlikely]] {
        JSTokenLocation spreadLocation(tokenLocation());
        auto start = m_token.m_startPosition;
        auto divot = m_token.m_endPosition;
        next();
        TreeExpression spreadExpr = parseAssignmentExpression(context);
        propagateError();
        auto end = m_lastTokenEndPosition;
        type = ArgumentType::Spread;
        return context.createSpreadExpression(spreadLocation, spreadExpr, start, divot, end);
    }

    type = ArgumentType::Normal;
    return parseAssignmentExpression(context);
}

IGNORE_GCC_WARNINGS_BEGIN("unused-but-set-parameter")
template <typename TreeBuilder, typename ParserType>
static inline void recordCallOrApplyDepth(ParserType* parser, VM& vm, std::optional<typename ParserType::CallOrApplyDepthScope>& callOrApplyDepthScope, typename TreeBuilder::Expression expression)
{
    if constexpr (std::is_same_v<TreeBuilder, ASTBuilder>) {
        if (expression->isDotAccessorNode()) {
            DotAccessorNode* dot = static_cast<DotAccessorNode*>(expression);
            bool isCallOrApply = dot->identifier() == vm.propertyNames->builtinNames().callPublicName() || dot->identifier() == vm.propertyNames->builtinNames().applyPublicName();
            if (isCallOrApply)
                callOrApplyDepthScope.emplace(parser);
        }
    }
}
IGNORE_GCC_WARNINGS_END

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseMemberExpression(TreeBuilder& context)
{
    TreeExpression base = 0;
    JSTextPosition expressionStart = tokenStartPosition();
    JSTokenLocation location = tokenLocation();
    Vector<JSTextPosition, 4> newTokenStartPositions;
    while (match(NEW)) {
        newTokenStartPositions.append(tokenStartPosition());
        next();
    }
    size_t newCount = newTokenStartPositions.size();

    bool baseIsSuper = match(SUPER);
    bool previousBaseWasSuper = false;
    bool baseIsImport = match(IMPORT);
    bool baseIsAsyncKeyword = false;

    if (newCount && consume(DOT)) {
        if (matchContextualKeyword(m_vm.propertyNames->target)) [[likely]] {
            ScopeRef closestOrdinaryFunctionScope = closestParentOrdinaryFunctionNonLexicalScope();
            bool isClassFieldInitializer = m_parserState.isParsingClassFieldInitializer;
            bool isFunctionEvalContextType = m_isInsideOrdinaryFunction && (closestOrdinaryFunctionScope->evalContextType() == EvalContextType::FunctionEvalContext || closestOrdinaryFunctionScope->evalContextType() == EvalContextType::InstanceFieldEvalContext);
            semanticFailIfFalse(currentScope()->isFunction() || currentScope()->isStaticBlock() || isFunctionEvalContextType || isClassFieldInitializer, "new.target is only valid inside functions or static blocks");
            if (currentScope()->isArrowFunction()) {
                semanticFailIfFalse(!closestOrdinaryFunctionScope->isGlobalCode() || isFunctionEvalContextType || isClassFieldInitializer, "new.target is not valid inside arrow functions in global code");
                currentScope()->setInnerArrowFunctionUsesNewTarget();
            }
            base = context.createNewTargetExpr(location);
            newCount--;
            next();
        } else {
            failIfTrue(match(IDENT), "\"new.\" can only be followed with target");
            failDueToUnexpectedToken();
        }
    } else if (baseIsSuper) {
        ScopeRef closestOrdinaryFunctionScope = closestParentOrdinaryFunctionNonLexicalScope();
        ScopeRef classScope = closestClassScopeOrTopLevelScope();
        bool isClassFieldInitializer = classScope.index() > closestOrdinaryFunctionScope.index();
        semanticFailIfFalse(currentScope()->isFunction() || isClassFieldInitializer || (closestOrdinaryFunctionScope->isEvalContext() && closestOrdinaryFunctionScope->expectedSuperBinding() == SuperBinding::Needed), "super is not valid in this context");
        base = context.createSuperExpr(location);
        next();
        failIfTrue(match(OPENPAREN) && currentScope()->evalContextType() == EvalContextType::InstanceFieldEvalContext, "super call is not valid in this context");
        ScopeRef functionScope = currentFunctionScope();
        functionScope->setNeedsSuperBinding();
        // It unnecessary to check of using super during reparsing one more time. Also it can lead to syntax error
        // in case of arrow function because during reparsing we don't know whether we currently parse the arrow function
        // inside of the constructor or method.
        if (!m_lexer->isReparsingFunction()) {
            SuperBinding functionSuperBinding = !functionScope->isArrowFunction() && !closestOrdinaryFunctionScope->isEvalContext()
                ? functionScope->expectedSuperBinding()
                : closestOrdinaryFunctionScope->expectedSuperBinding();
            semanticFailIfTrue(functionSuperBinding == SuperBinding::NotNeeded && !isClassFieldInitializer, "super is not valid in this context");
        }
    } else if (baseIsImport) {
        next();
        JSTextPosition expressionEnd = lastTokenEndPosition();
        if (consume(DOT)) {
            if (matchContextualKeyword(m_vm.propertyNames->builtinNames().metaPublicName())) [[likely]] {
                semanticFailIfFalse(m_scriptMode == JSParserScriptMode::Module, "import.meta is only valid inside modules");
                base = context.createImportMetaExpr(location, createResolveAndUseVariable(context, &m_vm.propertyNames->metaPrivateName, false, expressionStart, location));
                currentScope()->setUsesImportMeta();
                next();
            } else {
                failIfTrue(match(IDENT), "\"import.\" can only be followed with meta");
                failDueToUnexpectedToken();
            }
        } else {
            semanticFailIfTrue(newCount, "Cannot use new with import");
            consumeOrFail(OPENPAREN, "import call expects one or two arguments");
            SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
            TreeExpression expr = parseAssignmentExpression(context);
            failIfFalse(expr, "Cannot parse expression");
            TreeExpression optionExpression = 0;
            if (consume(COMMA)) {
                if (!match(CLOSEPAREN)) {
                    optionExpression = parseAssignmentExpression(context);
                    failIfFalse(optionExpression, "Cannot parse expression");
                    consume(COMMA);
                }
            }
            consumeOrFail(CLOSEPAREN, "import call expects one or two arguments");
            base = context.createImportExpr(location, expr, optionExpression, expressionStart, expressionEnd, lastTokenEndPosition());
        }
    } else {
        const bool isAsync = matchContextualKeyword(m_vm.propertyNames->async);

        if (TreeExpression argumentsDotLengthExpression = tryParseArgumentsDotLengthForFastPath(context))
            base = argumentsDotLengthExpression;
        else
            base = parsePrimaryExpression(context);
        failIfFalse(base, "Cannot parse base expression");
        if (isAsync && context.isResolve(base) && !m_lexer->hasLineTerminatorBeforeToken()) [[unlikely]] {
            if (matchSpecIdentifier()) [[unlikely]] {
                // AsyncArrowFunction
                forceClassifyExpressionError(ErrorIndicatesAsyncArrowFunction);
                failDueToUnexpectedToken();
            }
            baseIsAsyncKeyword = true;
        }
    }

    failIfFalse(base, "Cannot parse base expression");

    do {
        TreeExpression optionalChainBase = 0;
        JSTokenLocation optionalChainLocation;
        bool isOptionalCall = false;
        JSTokenType type = m_token.m_type;

        if (match(QUESTIONDOT)) {
            semanticFailIfTrue(newCount, "Cannot call constructor in an optional chain");
            semanticFailIfTrue(baseIsSuper, "Cannot use super as the base of an optional chain");
            optionalChainBase = base;
            optionalChainLocation = tokenLocation();

            SavePoint savePoint = createSavePoint(context);
            next();
            if (match(OPENBRACKET) || match(OPENPAREN) || match(BACKQUOTE))
                type = m_token.m_type;
            else {
                type = DOT;
                restoreSavePoint(context, savePoint);
            }
        }

        while (true) {
            switch (type) {
            case OPENBRACKET: {
                m_parserState.nonTrivialExpressionCount++;
                JSTextPosition expressionDivot = tokenStartPosition();
                next();
                SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
                int initialAssignments = m_parserState.assignmentCount;
                TreeExpression property = parseExpression(context);
                failIfFalse(property, "Cannot parse subscript expression");
                base = context.createBracketAccess(location, base, property, initialAssignments != m_parserState.assignmentCount, expressionStart, expressionDivot, tokenEndPosition());

                if (baseIsSuper && currentScope()->isArrowFunction()) [[unlikely]]
                    currentFunctionScope()->setInnerArrowFunctionUsesSuperProperty();

                handleProductionOrFail(CLOSEBRACKET, "]", "end", "subscript expression");
                break;
            }
            case OPENPAREN: {
                if (baseIsSuper)
                    failIfTrue(m_parserState.isParsingClassFieldInitializer, "super call is not valid in class field initializer context");
                m_parserState.nonTrivialExpressionCount++;
                SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
                if (newCount) {
                    newCount--;
                    semanticFailIfTrue(baseIsSuper, "Cannot use new with super call");
                    JSTextPosition expressionEnd = lastTokenEndPosition();
                    TreeArguments arguments = parseArguments(context);
                    failIfFalse(arguments, "Cannot parse call arguments");
                    base = context.createNewExpr(location, base, arguments, expressionStart, expressionEnd, lastTokenEndPosition());
                } else {
                    size_t usedVariablesSize = currentScope()->currentUsedVariablesSize();
                    JSTextPosition expressionEnd = lastTokenEndPosition();
                    std::optional<CallOrApplyDepthScope> callOrApplyDepthScope;
                    recordCallOrApplyDepth<TreeBuilder>(this, m_vm, callOrApplyDepthScope, base);

                    TreeArguments arguments = parseArguments(context);

                    if (baseIsAsyncKeyword && (!arguments || match(ARROWFUNCTION))) [[unlikely]] {
                        currentScope()->revertToPreviousUsedVariables(usedVariablesSize);
                        forceClassifyExpressionError(ErrorIndicatesAsyncArrowFunction);
                        failDueToUnexpectedToken();
                    }

                    failIfFalse(arguments, "Cannot parse call arguments");
                    if (baseIsSuper) {
                        ScopeRef functionScope = currentFunctionScope();
                        functionScope->setHasDirectSuper();
                        // It unnecessary to check of using super during reparsing one more time. Also it can lead to syntax error
                        // in case of arrow function because during reparsing we don't know whether we currently parse the arrow function
                        // inside of the constructor or method.
                        if (!m_lexer->isReparsingFunction()) {
                            ScopeRef closestOrdinaryFunctionScope = closestParentOrdinaryFunctionNonLexicalScope();
                            semanticFailIfFalse(closestOrdinaryFunctionScope->constructorKind() == ConstructorKind::Extends || (closestOrdinaryFunctionScope->isEvalContext() && closestOrdinaryFunctionScope->derivedContextType() == DerivedContextType::DerivedConstructorContext), "super is not valid in this context");
                        }
                        if (currentScope()->isArrowFunction())
                            functionScope->setInnerArrowFunctionUsesSuperCall();
                    }

                    isOptionalCall = optionalChainLocation.endOffset == static_cast<unsigned>(expressionEnd.offset);
                    base = context.makeFunctionCallNode(location, base, previousBaseWasSuper, arguments, expressionStart,
                        expressionEnd, lastTokenEndPosition(), callOrApplyDepthScope ? callOrApplyDepthScope->distanceToInnermostChild() : 0, isOptionalCall);
                }
                break;
            }
            case DOT: {
                m_parserState.nonTrivialExpressionCount++;
                JSTextPosition expressionDivot = tokenStartPosition();
                nextExpectIdentifier(TreeBuilder::DontBuildKeywords | LexerFlags::IgnoreReservedWords);
                const Identifier* ident = m_token.m_data.ident;
                auto type = DotType::Name;
                if (match(PRIVATENAME)) {
                    ASSERT(ident);
                    failIfTrue(baseIsSuper, "Cannot access private names from super");
                    if (currentScope()->evalContextType() == EvalContextType::InstanceFieldEvalContext) [[unlikely]]
                        semanticFailIfFalse(currentScope()->hasPrivateName(*ident), "Cannot reference undeclared private field '", ident->impl(), "'");
                    currentScope()->usePrivateName(*ident);
                    m_seenPrivateNameUseInNonReparsingFunctionMode = true;
                    m_parserState.lastPrivateName = ident;
                    type = DotType::PrivateMember;
                    m_token.m_type = IDENT;
                }
                matchOrFail(IDENT, "Expected a property name after ", optionalChainBase ? "'?.'" : "'.'");
                base = context.createDotAccess(location, base, ident, type, expressionStart, expressionDivot, tokenEndPosition());
                if (baseIsSuper && currentScope()->isArrowFunction()) [[unlikely]]
                    currentFunctionScope()->setInnerArrowFunctionUsesSuperProperty();
                next();
                break;
            }
            case BACKQUOTE: {
                semanticFailIfTrue(optionalChainBase, "Cannot use tagged templates in an optional chain");
                semanticFailIfTrue(baseIsSuper, "Cannot use super as tag for tagged templates");
                JSTextPosition expressionDivot = tokenStartPosition();
                SetForScope nonLHSCountScope(m_parserState.nonLHSCount);
                typename TreeBuilder::TemplateLiteral templateLiteral = parseTemplateLiteral(context, LexerType::RawStringsBuildMode::BuildRawStrings);
                failIfFalse(templateLiteral, "Cannot parse template literal");
                base = context.createTaggedTemplate(location, base, templateLiteral, expressionStart, expressionDivot, lastTokenEndPosition());
                m_seenTaggedTemplateInNonReparsingFunctionMode = true;
                break;
            }
            default:
                goto endOfChain;
            }
            previousBaseWasSuper = baseIsSuper;
            baseIsSuper = false;
            type = m_token.m_type;
        }
endOfChain:
        if (optionalChainBase)
            base = context.createOptionalChain(location, isOptionalCall ? 0 : optionalChainBase, base, !match(QUESTIONDOT));
    } while (match(QUESTIONDOT));

    semanticFailIfTrue(baseIsSuper, newCount ? "Cannot use new with super call" : "super is not valid in this context");
    while (newCount--)
        base = context.createNewExpr(location, base, expressionStart, newTokenStartPositions[newCount], lastTokenEndPosition());
    return base;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseArrowFunctionExpression(TreeBuilder& context, bool isAsync, const JSTokenLocation& location)
{
    ParserFunctionInfo<TreeBuilder> info;
    info.name = &m_vm.propertyNames->nullIdentifier;

    SetForScope innerParseMode(m_parseMode, isAsync ? SourceParseMode::AsyncArrowFunctionMode : SourceParseMode::ArrowFunctionMode);
    failIfFalse((parseFunctionInfo(context, FunctionNameRequirements::Unnamed, true, ConstructorKind::None, SuperBinding::NotNeeded, location.startOffset, info, FunctionDefinitionType::Expression)), "Cannot parse arrow function expression");

    return context.createArrowFunctionExpr(location, info);
}

static const char* operatorString(bool prefix, unsigned tok)
{
    switch (tok) {
    case MINUSMINUS:
    case AUTOMINUSMINUS:
        return prefix ? "prefix-decrement" : "decrement";

    case PLUSPLUS:
    case AUTOPLUSPLUS:
        return prefix ? "prefix-increment" : "increment";

    case EXCLAMATION:
        return "logical-not";

    case TILDE:
        return "bitwise-not";
    
    case TYPEOF:
        return "typeof";
    
    case VOIDTOKEN:
        return "void";
    
    case DELETETOKEN:
        return "delete";
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "error";
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseUnaryExpression(TreeBuilder& context)
{
    typename TreeBuilder::UnaryExprContext unaryExprContext(context);
    AllowInOverride allowInOverride(this);
    int tokenStackDepth = 0;
    bool hasPrefixUpdateOp = false;
    unsigned lastOperator = 0;

    if (match(AWAIT) && !m_parserState.classFieldInitMasksAsync && (currentFunctionScope()->isAsyncFunctionBoundary() || isModuleParseMode(sourceParseMode()))) [[unlikely]] {
        semanticFailIfTrue(currentScope()->isStaticBlock(), "Cannot use 'await' within static block");
        return parseAwaitExpression(context);
    }

    JSTokenLocation location(tokenLocation());

    int oldTokenStackDepth = context.unaryTokenStackDepth();
    auto scopeExit = makeScopeExit([&] {
        ASSERT_UNUSED(oldTokenStackDepth, oldTokenStackDepth <= context.unaryTokenStackDepth());
    });

    while (isUnaryOp(m_token.m_type)) {
        semanticFailIfTrue(hasPrefixUpdateOp, "The ", operatorString(true, lastOperator), " operator requires a reference expression");
        if (isUpdateOp(m_token.m_type))
            hasPrefixUpdateOp = true;
        lastOperator = m_token.m_type;
        m_parserState.nonLHSCount++;
        context.appendUnaryToken(tokenStackDepth, m_token.m_type, tokenStartPosition());
        next();
        m_parserState.nonTrivialExpressionCount++;
    }
    JSTextPosition subExprStart = tokenStartPosition();
    ASSERT(subExprStart.offset >= subExprStart.lineStartOffset);
    TreeExpression expr = parseMemberExpression(context);
    if (!expr) [[unlikely]] {
        failIfTrue(lastOperator, "Cannot parse subexpression of ", operatorString(true, lastOperator), "operator");
        failWithMessage("Cannot parse member expression");
    }
    if constexpr (std::is_same_v<TreeBuilder, ASTBuilder>)
        ASSERT(oldTokenStackDepth + tokenStackDepth == context.unaryTokenStackDepth());
    if (isUpdateOp(static_cast<JSTokenType>(lastOperator))) {
        semanticFailIfTrue(context.isMetaProperty(expr), metaPropertyName(context, expr), " can't come after a prefix operator");
        semanticFailIfFalse(isSimpleAssignmentTarget(context, expr), "Prefix ", lastOperator == PLUSPLUS || lastOperator == AUTOPLUSPLUS ? "++" : "--", " operator applied to value that is not a reference");
    }
    bool isEvalOrArguments = false;
    if (strictMode()) {
        if (context.isResolve(expr))
            isEvalOrArguments = *m_parserState.lastIdentifier == m_vm.propertyNames->eval || *m_parserState.lastIdentifier == m_vm.propertyNames->arguments;
    }
    failIfTrueIfStrict(isEvalOrArguments && hasPrefixUpdateOp, "Cannot modify '", m_parserState.lastIdentifier->impl(), "' in strict mode");
    switch (m_token.m_type) {
    case PLUSPLUS:
        semanticFailIfTrue(context.isMetaProperty(expr), metaPropertyName(context, expr), " can't come before a postfix operator");
        semanticFailIfFalse(isSimpleAssignmentTarget(context, expr), "Postfix ++ operator applied to value that is not a reference");
        m_parserState.nonTrivialExpressionCount++;
        m_parserState.nonLHSCount++;
        expr = context.makePostfixNode(location, expr, Operator::PlusPlus, subExprStart, lastTokenEndPosition(), tokenEndPosition());
        m_parserState.assignmentCount++;
        failIfTrueIfStrict(isEvalOrArguments, "Cannot modify '", m_parserState.lastIdentifier->impl(), "' in strict mode");
        semanticFailIfTrue(hasPrefixUpdateOp, "The ", operatorString(false, lastOperator), " operator requires a reference expression");
        next();
        break;
    case MINUSMINUS:
        semanticFailIfTrue(context.isMetaProperty(expr), metaPropertyName(context, expr), " can't come before a postfix operator");
        semanticFailIfFalse(isSimpleAssignmentTarget(context, expr), "Postfix -- operator applied to value that is not a reference");
        m_parserState.nonTrivialExpressionCount++;
        m_parserState.nonLHSCount++;
        expr = context.makePostfixNode(location, expr, Operator::MinusMinus, subExprStart, lastTokenEndPosition(), tokenEndPosition());
        m_parserState.assignmentCount++;
        failIfTrueIfStrict(isEvalOrArguments, "'", m_parserState.lastIdentifier->impl(), "' cannot be modified in strict mode");
        semanticFailIfTrue(hasPrefixUpdateOp, "The ", operatorString(false, lastOperator), " operator requires a reference expression");
        next();
        break;
    default:
        break;
    }
    
    JSTextPosition end = lastTokenEndPosition();
    while (tokenStackDepth) {
        subExprStart = context.unaryTokenStackLastStart(tokenStackDepth);
        auto tokenType = context.unaryTokenStackLastType(tokenStackDepth);
        switch (tokenType) {
        case EXCLAMATION:
            expr = context.createLogicalNot(location, expr);
            break;
        case TILDE:
            expr = context.makeBitwiseNotNode(location, expr);
            break;
        case MINUS:
            expr = context.makeNegateNode(location, expr);
            break;
        case PLUS:
            expr = context.createUnaryPlus(location, expr);
            break;
        case PLUSPLUS:
        case AUTOPLUSPLUS:
            ASSERT(isSimpleAssignmentTarget(context, expr));
            expr = context.makePrefixNode(location, expr, Operator::PlusPlus, subExprStart, subExprStart + 2, end);
            m_parserState.assignmentCount++;
            break;
        case MINUSMINUS:
        case AUTOMINUSMINUS:
            ASSERT(isSimpleAssignmentTarget(context, expr));
            expr = context.makePrefixNode(location, expr, Operator::MinusMinus, subExprStart, subExprStart + 2, end);
            m_parserState.assignmentCount++;
            break;
        case TYPEOF:
            expr = context.makeTypeOfNode(location, expr);
            break;
        case VOIDTOKEN:
            expr = context.createVoid(location, expr);
            break;
        case DELETETOKEN:
            failIfTrueIfStrict(context.isResolve(expr), "Cannot delete unqualified property '", m_parserState.lastIdentifier->impl(), "' in strict mode");
            semanticFailIfTrue(context.isPrivateLocation(expr), "Cannot delete private field ", m_parserState.lastPrivateName->impl());
            expr = context.makeDeleteNode(location, expr, context.unaryTokenStackLastStart(tokenStackDepth), end, end);
            break;
        default:
            // If we get here something has gone horribly horribly wrong
            CRASH();
        }
        context.unaryTokenStackRemoveLast(tokenStackDepth);
    }
    return expr;
}

template <typename LexerType> void Parser<LexerType>::printUnexpectedTokenText(WTF::PrintStream& out)
{
    switch (m_token.m_type) {
    case EOFTOK:
        out.print("Unexpected end of script");
        return;
    case UNTERMINATED_IDENTIFIER_ESCAPE_ERRORTOK:
    case UNTERMINATED_IDENTIFIER_UNICODE_ESCAPE_ERRORTOK:
        out.print("Incomplete unicode escape in identifier: '", getToken(), "'");
        return;
    case UNTERMINATED_MULTILINE_COMMENT_ERRORTOK:
        out.print("Unterminated multiline comment");
        return;
    case UNTERMINATED_NUMERIC_LITERAL_ERRORTOK:
        out.print("Unterminated numeric literal '", getToken(), "'");
        return;
    case UNTERMINATED_STRING_LITERAL_ERRORTOK:
        out.print("Unterminated string literal '", getToken(), "'");
        return;
    case INVALID_IDENTIFIER_ESCAPE_ERRORTOK:
        out.print("Invalid escape in identifier: '", getToken(), "'");
        return;
    case ESCAPED_KEYWORD:
        out.print("Unexpected escaped characters in keyword token: '", getToken(), "'");
        return;
    case INVALID_IDENTIFIER_UNICODE_ESCAPE_ERRORTOK:
        out.print("Invalid unicode escape in identifier: '", getToken(), "'");
        return;
    case INVALID_NUMERIC_LITERAL_ERRORTOK:
        out.print("Invalid numeric literal: '", getToken(), "'");
        return;
    case UNTERMINATED_OCTAL_NUMBER_ERRORTOK:
        out.print("Invalid use of octal: '", getToken(), "'");
        return;
    case INVALID_STRING_LITERAL_ERRORTOK:
        out.print("Invalid string literal: '", getToken(), "'");
        return;
    case INVALID_UNICODE_ENCODING_ERRORTOK:
        out.print("Invalid unicode encoding: '", getToken(), "'");
        return;
    case INVALID_IDENTIFIER_UNICODE_ERRORTOK:
        out.print("Invalid unicode code point in identifier: '", getToken(), "'");
        return;
    case ERRORTOK:
        out.print("Unrecognized token '", getToken(), "'");
        return;
    case STRING:
        out.print("Unexpected string literal ", getToken());
        return;
    case INTEGER:
    case DOUBLE:
        out.print("Unexpected number '", getToken(), "'");
        return;
    
    case RESERVED_IF_STRICT:
        out.print("Unexpected use of reserved word '", getToken(), "' in strict mode");
        return;
        
    case RESERVED:
        out.print("Unexpected use of reserved word '", getToken(), "'");
        return;

    case INVALID_PRIVATE_NAME_ERRORTOK:
        out.print("Invalid private name '", getToken(), "'");
        return;

    case PRIVATENAME:
        out.print("Unexpected private name ", getToken());
        return;

    case AWAIT:
    case IDENT:
        out.print("Unexpected identifier '", getToken(), "'");
        return;

    default:
        break;
    }

    if (m_token.m_type & KeywordTokenFlag) {
        out.print("Unexpected keyword '", getToken(), "'");
        return;
    }
    
    out.print("Unexpected token '", getToken(), "'");
}

// Instantiate the two flavors of Parser we need instead of putting most of this file in Parser.h
template class Parser<Lexer<LChar>>;
template class Parser<Lexer<char16_t>>;
    
} // namespace JSC

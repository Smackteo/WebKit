/*
 * Copyright (C) 2008-2024 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Cameron Zwarich <cwzwarich@uwaterloo.ca>
 * Copyright (C) 2012 Igalia, S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "BytecodeGeneratorBase.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "Instruction.h"
#include "Interpreter.h"
#include "JSAsyncGenerator.h"
#include "JSBigInt.h"
#include "JSGenerator.h"
#include "JSTemplateObjectDescriptor.h"
#include "Label.h"
#include "LabelScope.h"
#include "LinkTimeConstant.h"
#include "Nodes.h"
#include "ParserError.h"
#include "ProfileTypeBytecodeFlag.h"
#include "RegisterID.h"
#include "StaticPropertyAnalyzer.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedCodeBlockGenerator.h"
#include <functional>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashFunctions.h>
#include <wtf/SegmentedVector.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

    class JSImmutableButterfly;
    class Identifier;
    class ForInContext;

    enum ExpectedFunction {
        NoExpectedFunction,
        ExpectObjectConstructor,
        ExpectArrayConstructor
    };

    enum class EmitAwait : bool { No, Yes };

    enum class DebuggableCall : bool { No, Yes };
    enum class ThisResolutionType { Local, Scoped };
    enum class InvalidPrototypeMode : uint8_t { Throw, Ignore };
    enum class LinkTimeConstant : int32_t;
    
    class CallArguments {
    public:
        CallArguments(BytecodeGenerator&, ArgumentsNode*, unsigned additionalArguments = 0);

        RegisterID* thisRegister() { return m_argv[0].get(); }
        RegisterID* argumentRegister(unsigned i) { return m_argv[i + 1].get(); }
        unsigned stackOffset() { return -m_argv[0]->index() + CallFrame::headerSizeInRegisters; }
        unsigned argumentCountIncludingThis() { return m_argv.size(); }
        ArgumentsNode* argumentsNode() { return m_argumentsNode; }

    private:
        ArgumentsNode* m_argumentsNode;
        std::span<RefPtr<RegisterID>> m_argv;
        Vector<RefPtr<RegisterID>, 8, UnsafeVectorOverflow> m_allocatedRegisters;
    };

    class Variable {
    public:
        enum VariableKind { NormalVariable, SpecialVariable };

        Variable() = default;
        
        Variable(const Identifier& ident)
            : m_ident(ident)
            , m_local(nullptr)
            , m_attributes(0)
            , m_kind(NormalVariable) // This is somewhat meaningless here for this kind of Variable.
            , m_symbolTableConstantIndex(0) // This is meaningless here for this kind of Variable.
            , m_isLexicallyScoped(false)
        {
        }

        Variable(const Identifier& ident, VarOffset offset, RegisterID* local, unsigned attributes, VariableKind kind, int symbolTableConstantIndex, bool isLexicallyScoped)
            : m_ident(ident)
            , m_offset(offset)
            , m_local(local)
            , m_attributes(attributes)
            , m_kind(kind)
            , m_symbolTableConstantIndex(symbolTableConstantIndex)
            , m_isLexicallyScoped(isLexicallyScoped)
        {
        }

        // If it's unset, then it is a non-locally-scoped variable. If it is set, then it could be
        // a stack variable, a scoped variable in a local scope, or a variable captured in the
        // direct arguments object.
        bool isResolved() const { return !!m_offset; }
        int symbolTableConstantIndex() const { ASSERT(isResolved() && !isSpecial()); return m_symbolTableConstantIndex; }
        
        const Identifier& ident() const { return m_ident; }
        
        VarOffset offset() const { return m_offset; }
        bool isLocal() const { return m_offset.isStack(); }
        RegisterID* local() const { return m_local; }

        bool isReadOnly() const { return m_attributes & PropertyAttribute::ReadOnly; }
        bool isSpecial() const { return m_kind != NormalVariable; }
        bool isConst() const { return isReadOnly() && m_isLexicallyScoped; }
        void setIsReadOnly() { m_attributes |= PropertyAttribute::ReadOnly; }

        void dump(PrintStream&) const;

        friend bool operator==(const Variable&, const Variable&) = default;

    private:
        Identifier m_ident;
        VarOffset m_offset { };
        RegisterID* m_local { nullptr };
        unsigned m_attributes { 0 };
        VariableKind m_kind { NormalVariable };
        int m_symbolTableConstantIndex { 0 }; // This is meaningless here for this default NormalVariable kind of Variable.
        bool m_isLexicallyScoped { false };
    };

    // https://tc39.github.io/ecma262/#sec-completion-record-specification-type
    //
    // For the Break and Continue cases, instead of using the Break and Continue enum values
    // below, we use the unique jumpID of the break and continue statement as the encoding
    // for the CompletionType value. emitFinallyCompletion() uses this jumpID value later
    // to determine the appropriate jump target to jump to after executing the relevant finally
    // blocks. The jumpID is computed as:
    //     jumpID = bytecodeOffset (of the break/continue node) + CompletionType::NumberOfTypes.
    // Hence, there won't be any collision between jumpIDs and CompletionType enums.
    enum class CompletionType : int {
        Normal,
        Throw,
        Return,
        NumberOfTypes
    };

    inline CompletionType bytecodeOffsetToJumpID(unsigned offset)
    {
        int jumpIDAsInt = offset + static_cast<int>(CompletionType::NumberOfTypes);
        ASSERT(jumpIDAsInt >= static_cast<int>(CompletionType::NumberOfTypes));
        return static_cast<CompletionType>(jumpIDAsInt);
    }

    struct FinallyJump {
        FinallyJump(CompletionType jumpID, int targetLexicalScopeIndex, Label& targetLabel)
            : jumpID(jumpID)
            , targetLexicalScopeIndex(targetLexicalScopeIndex)
            , targetLabel(targetLabel)
        { }

        CompletionType jumpID;
        int targetLexicalScopeIndex;
        Ref<Label> targetLabel;
    };

    class FinallyContext {
    public:
        FinallyContext() { }
        FinallyContext(BytecodeGenerator&, Label& finallyLabel);

        FinallyContext* outerContext() const { return m_outerContext; }
        Label* finallyLabel() const { return m_finallyLabel; }

        RegisterID* completionTypeRegister() const { return m_completionRecord.typeRegister.get(); }
        RegisterID* completionValueRegister() const { return m_completionRecord.valueRegister.get(); }

        uint32_t numberOfBreaksOrContinues() const { return m_numberOfBreaksOrContinues; }
        void incNumberOfBreaksOrContinues() { m_numberOfBreaksOrContinues++; }

        bool handlesReturns() const { return m_handlesReturns; }
        void setHandlesReturns() { m_handlesReturns = true; }

        void registerJump(CompletionType jumpID, int lexicalScopeIndex, Label& targetLabel)
        {
            m_jumps.append(FinallyJump(jumpID, lexicalScopeIndex, targetLabel));
        }

        size_t numberOfJumps() const { return m_jumps.size(); }
        FinallyJump& jumps(size_t i) { return m_jumps[i]; }

    private:
        FinallyContext* m_outerContext { nullptr };
        Label* m_finallyLabel { nullptr };
        Checked<uint32_t, WTF::CrashOnOverflow> m_numberOfBreaksOrContinues;
        bool m_handlesReturns { false };
        Vector<FinallyJump> m_jumps;
        struct {
            RefPtr<RegisterID> typeRegister;
            RefPtr<RegisterID> valueRegister;
        } m_completionRecord;
    };

    struct ControlFlowScope {
        typedef uint8_t Type;
        enum {
            Label,
            Finally
        };
        ControlFlowScope(Type type, int lexicalScopeIndex, FinallyContext* finallyContext = nullptr)
            : type(type)
            , lexicalScopeIndex(lexicalScopeIndex)
            , finallyContext(finallyContext)
        { }

        bool isLabelScope() const { return type == Label; }
        bool isFinallyScope() const { return type == Finally; }

        Type type;
        int lexicalScopeIndex;
        FinallyContext* finallyContext;
    };

    class ForInContext : public RefCounted<ForInContext> {
        WTF_MAKE_TZONE_ALLOCATED(ForInContext);
        WTF_MAKE_NONCOPYABLE(ForInContext);
    public:
        using GetInst = std::tuple<unsigned, int>;
        using PutInst = GetInst;
        using InInst = GetInst;
        using HasOwnPropertyJumpInst = std::tuple<unsigned, unsigned>;

        bool isValid() const { return m_isValid; }
        void invalidate() { m_isValid = false; }

        RegisterID* local() const { return m_localRegister.get(); }
        RegisterID* propertyName() const { return m_propertyName.get(); }
        RegisterID* propertyOffset() const { return m_propertyOffset.get(); }
        RegisterID* enumerator() const { return m_enumerator.get(); }
        RegisterID* mode() const { return m_mode.get(); }
        const std::optional<Variable>& baseVariable() const { return m_baseVariable; }

        void addGetInst(unsigned instIndex, int propertyRegIndex)
        {
            m_getInsts.append(GetInst { instIndex, propertyRegIndex });
        }

        void addPutInst(unsigned instIndex, int propertyRegIndex)
        {
            m_putInsts.append(PutInst { instIndex, propertyRegIndex });
        }

        void addInInst(unsigned instIndex, int propertyRegIndex)
        {
            m_inInsts.append(InInst { instIndex, propertyRegIndex });
        }

        void addHasOwnPropertyJump(unsigned branchInstIndex, unsigned genericPathTarget)
        {
            m_hasOwnPropertyJumpInsts.append(HasOwnPropertyJumpInst { branchInstIndex, genericPathTarget });
        }

        ForInContext(RegisterID* localRegister, RegisterID* propertyName, RegisterID* propertyOffset, RegisterID* enumerator, RegisterID* mode, std::optional<Variable> baseVariable, unsigned bodyBytecodeStartOffset)
            : m_localRegister(localRegister)
            , m_propertyName(propertyName)
            , m_propertyOffset(propertyOffset)
            , m_enumerator(enumerator)
            , m_mode(mode)
            , m_baseVariable(baseVariable)
            , m_bodyBytecodeStartOffset(bodyBytecodeStartOffset)
        { }

        unsigned bodyBytecodeStartOffset() const { return m_bodyBytecodeStartOffset; }

        void finalize(BytecodeGenerator&, UnlinkedCodeBlockGenerator*, unsigned bodyBytecodeEndOffset);

    private:
        RefPtr<RegisterID> m_localRegister;
        RefPtr<RegisterID> m_propertyName;
        RefPtr<RegisterID> m_propertyOffset;
        RefPtr<RegisterID> m_enumerator;
        RefPtr<RegisterID> m_mode;
        std::optional<Variable> m_baseVariable;
        bool m_isValid { true };
        unsigned m_bodyBytecodeStartOffset;
        Vector<InInst> m_inInsts;
        Vector<GetInst> m_getInsts;
        Vector<PutInst> m_putInsts;
        Vector<HasOwnPropertyJumpInst> m_hasOwnPropertyJumpInsts;
    };

    struct TryData {
        Ref<Label> target;
        HandlerType handlerType;
    };

    struct TryContext {
        Ref<Label> start;
        TryData* tryData;
    };

    struct TryRange {
        Ref<Label> start;
        Ref<Label> end;
        TryData* tryData;
    };


    struct JSGeneratorTraits {
        using OpcodeTraits = JSOpcodeTraits;
        using OpcodeID = ::JSC::OpcodeID;
        using OpNop = ::JSC::OpNop;
        using CodeBlock = std::unique_ptr<UnlinkedCodeBlockGenerator>;
        using InstructionType = JSInstruction;
        static constexpr OpcodeID opcodeForDisablingOptimizations = op_end;
    };

    class BytecodeGenerator : public BytecodeGeneratorBase<JSGeneratorTraits> {
        WTF_MAKE_TZONE_ALLOCATED(BytecodeGenerator);
        WTF_MAKE_NONCOPYABLE(BytecodeGenerator);

        friend class FinallyContext;
        friend class ForInContext;
        friend class StrictModeScope;

        template <typename OldOpType, typename NewOpType, typename TupleType>
        friend void rewriteOp(BytecodeGenerator&, TupleType&);

    public:
        typedef DeclarationStacks::FunctionStack FunctionStack;

        BytecodeGenerator(VM&, ProgramNode*, UnlinkedProgramCodeBlock*, OptionSet<CodeGenerationMode>, const RefPtr<TDZEnvironmentLink>&, const FixedVector<Identifier>*, const PrivateNameEnvironment*);
        BytecodeGenerator(VM&, FunctionNode*, UnlinkedFunctionCodeBlock*, OptionSet<CodeGenerationMode>, const RefPtr<TDZEnvironmentLink>&, const FixedVector<Identifier>*, const PrivateNameEnvironment*);
        BytecodeGenerator(VM&, EvalNode*, UnlinkedEvalCodeBlock*, OptionSet<CodeGenerationMode>, const RefPtr<TDZEnvironmentLink>&, const FixedVector<Identifier>*, const PrivateNameEnvironment*);
        BytecodeGenerator(VM&, ModuleProgramNode*, UnlinkedModuleProgramCodeBlock*, OptionSet<CodeGenerationMode>, const RefPtr<TDZEnvironmentLink>&, const FixedVector<Identifier>*, const PrivateNameEnvironment*);

        ~BytecodeGenerator();
        
        VM& vm() const { return m_vm; }
        ParserArena& parserArena() const { return m_scopeNode->parserArena(); }
        const CommonIdentifiers& propertyNames() const { return *m_vm.propertyNames; }

        bool isConstructor() const { return m_codeBlock->isConstructor(); }
        DerivedContextType derivedContextType() const { return m_derivedContextType; }
        bool usesArrowFunction() const { return m_scopeNode->usesArrowFunction(); }
        bool needsToUpdateArrowFunctionContext() const { return m_needsToUpdateArrowFunctionContext; }
        bool usesEval() const { return m_scopeNode->usesEval(); }
        bool usesThis() const { return m_scopeNode->usesThis(); }
        bool isFunctionNode() const { return m_scopeNode->isFunctionNode(); }
        bool hasShadowsArgumentsCodeFeature() const { return m_scopeNode->hasShadowsArgumentsFeature(); }
        LexicallyScopedFeatures lexicallyScopedFeatures() const { return m_scopeNode->lexicallyScopedFeatures(); }
        PrivateBrandRequirement privateBrandRequirement() const { return m_codeBlock->privateBrandRequirement(); }
        ConstructorKind constructorKind() const { return m_codeBlock->constructorKind(); }
        SuperBinding superBinding() const { return m_codeBlock->superBinding(); }
        JSParserScriptMode scriptMode() const { return m_codeBlock->scriptMode(); }
        NeedsClassFieldInitializer needsClassFieldInitializer() const { return m_codeBlock->needsClassFieldInitializer(); }

        template<typename Node, typename UnlinkedCodeBlock>
        static ParserError generate(VM& vm, Node* node, const SourceCode& sourceCode, UnlinkedCodeBlock* unlinkedCodeBlock, OptionSet<CodeGenerationMode> codeGenerationMode, const RefPtr<TDZEnvironmentLink>& parentScopeTDZVariables, const FixedVector<Identifier>* generatorOrAsyncWrapperFunctionParameterNames, const PrivateNameEnvironment* privateNameEnvironment)
        {
            MonotonicTime before;
            if (Options::reportBytecodeCompileTimes()) [[unlikely]]
                before = MonotonicTime::now();

            DeferGC deferGC(vm);
            auto bytecodeGenerator = makeUnique<BytecodeGenerator>(vm, node, unlinkedCodeBlock, codeGenerationMode, parentScopeTDZVariables, generatorOrAsyncWrapperFunctionParameterNames, privateNameEnvironment);
            unsigned size;
            auto result = bytecodeGenerator->generate(size);

            if (Options::reportBytecodeCompileTimes()) [[unlikely]] {
                MonotonicTime after = MonotonicTime::now();
                dataLogLn(result.isValid() ? "Failed to compile #" : "Compiled #", CodeBlockHash(sourceCode, unlinkedCodeBlock->isConstructor() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall), " into bytecode ", size, " instructions in ", (after - before).milliseconds(), " ms.");
            }
            return result;
        }

        bool isArgumentNumber(const Identifier&, int);

        Variable variable(const Identifier&, ThisResolutionType = ThisResolutionType::Local);
        
        enum ExistingVariableMode { VerifyExisting, IgnoreExisting };
        void createVariable(const Identifier&, VarKind, SymbolTable*, ExistingVariableMode = VerifyExisting); // Creates the variable, or asserts that the already-created variable is sufficiently compatible.
        
        // Returns the register storing "this"
        RegisterID* thisRegister() { return &m_thisRegister; }
        RegisterID* argumentsRegister() { return m_argumentsRegister; }
        RegisterID* newTarget()
        {
            ASSERT(m_newTargetRegister);
            return m_newTargetRegister;
        }

        RegisterID* scopeRegister() { return m_scopeRegister; }

        RegisterID* generatorRegister() { return m_generatorRegister; }

        RegisterID* promiseRegister() { return m_promiseRegister; }

        // The same as newTemporary(), but this function returns "suggestion" if
        // "suggestion" is a temporary. This function is helpful in situations
        // where you've put "suggestion" in a RefPtr, but you'd like to allow
        // the next instruction to overwrite it anyway.
        RegisterID* newTemporaryOr(RegisterID* suggestion) { return suggestion->isTemporary() ? suggestion : newTemporary(); }

        // Functions for handling of dst register

        RegisterID* ignoredResult() { return &m_ignoredResultRegister; }

        // This will be allocated in the temporary region of registers, but it will
        // not be marked as a temporary. This will ensure that finalDestination() does
        // not overwrite a block scope variable that it mistakes as a temporary. These
        // registers can be (and are) reclaimed when the lexical scope they belong to
        // is no longer on the symbol table stack.
        RegisterID* newBlockScopeVariable();

        // Returns a place to write intermediate values of an operation
        // which reuses dst if it is safe to do so.
        RegisterID* tempDestination(RegisterID* dst)
        {
            return (dst && dst != ignoredResult() && dst->isTemporary()) ? dst : newTemporary();
        }

        // Returns the place to write the final output of an operation.
        RegisterID* finalDestination(RegisterID* originalDst, RegisterID* tempDst = nullptr)
        {
            if (originalDst && originalDst != ignoredResult())
                return originalDst;
            ASSERT(tempDst != ignoredResult());
            if (tempDst && tempDst->isTemporary())
                return tempDst;
            return newTemporary();
        }

        RegisterID* destinationForAssignResult(RegisterID* dst)
        {
            if (dst && dst != ignoredResult())
                return dst->isTemporary() ? dst : newTemporary();
            return nullptr;
        }

        // Moves src to dst if dst is not null and is different from src, otherwise just returns src.
        RegisterID* move(RegisterID* dst, RegisterID* src)
        {
            return dst == ignoredResult() ? nullptr : (dst && dst != src) ? emitMove(dst, src) : src;
        }

        Ref<LabelScope> newLabelScope(LabelScope::Type, const Identifier* = nullptr);

        void emitNode(RegisterID* dst, StatementNode* n)
        {
            SetForScope tailPositionPoisoner(m_allowTailCallOptimization, false);
            SetForScope callIgnoreResultPositionPoisoner(m_allowCallIgnoreResultOptimization, false);
            return emitNodeInTailPosition(dst, n);
        }

        void emitNodeInIgnoreResultPosition(StatementNode* n)
        {
            SetForScope tailPositionPoisoner(m_allowTailCallOptimization, false);
            SetForScope callIgnoreResultPositionPoisoner(m_allowCallIgnoreResultOptimization, m_defaultAllowCallIgnoreResultOptimization); // Revert it to default value.
            return emitNodeInTailPosition(ignoredResult(), n);
        }

        void emitNodeInTailPosition(RegisterID* dst, StatementNode* n)
        {
            // Node::emitCode assumes that dst, if provided, is either a local or a referenced temporary.
            ASSERT(!dst || dst == ignoredResult() || !dst->isTemporary() || dst->refCount());
            if (!m_vm.isSafeToRecurse()) [[unlikely]] {
                emitThrowExpressionTooDeepException();
                return;
            }
            if (n->needsDebugHook()) [[unlikely]]
                emitDebugHook(n);
            n->emitBytecode(*this, dst);
        }

        ALWAYS_INLINE unsigned addMetadataFor(OpcodeID opcodeID)
        {
            return m_codeBlock->metadata().addEntry(opcodeID);
        }

        ALWAYS_INLINE unsigned nextValueProfileIndex()
        {
            return m_codeBlock->metadata().addValueProfile();
        }

        void emitNode(StatementNode* n)
        {
            emitNode(nullptr, n);
        }

        void emitNodeInTailPosition(StatementNode* n)
        {
            emitNodeInTailPosition(nullptr, n);
        }

        RegisterID* emitNode(RegisterID* dst, ExpressionNode* n)
        {
            SetForScope tailPositionPoisoner(m_allowTailCallOptimization, false);
            SetForScope callIgnoreResultPositionPoisoner(m_allowCallIgnoreResultOptimization, false);
            return emitNodeInTailPosition(dst, n);
        }

        RegisterID* emitNodeInTailPositionFromReturnNode(RegisterID* dst, ExpressionNode* n)
        {
            SetForScope callIgnoreResultPositionPoisoner(m_allowCallIgnoreResultOptimization, false);
            return emitNodeInTailPosition(dst, n);
        }

        RegisterID* emitNodeInTailPositionFromExprStatementNode(RegisterID* dst, ExpressionNode* n)
        {
            SetForScope tailPositionPoisoner(m_allowTailCallOptimization, false);
            return emitNodeInTailPosition(dst, n);
        }

        RegisterID* emitNodeInIgnoreResultPosition(ExpressionNode* n)
        {
            SetForScope tailPositionPoisoner(m_allowTailCallOptimization, false);
            SetForScope callIgnoreResultPositionPoisoner(m_allowCallIgnoreResultOptimization, m_defaultAllowCallIgnoreResultOptimization); // Revert it to default value.
            return emitNodeInTailPosition(ignoredResult(), n);
        }

        RegisterID* emitNodeInTailPosition(RegisterID* dst, ExpressionNode* n)
        {
            // Node::emitCode assumes that dst, if provided, is either a local or a referenced temporary.
            ASSERT(!dst || dst == ignoredResult() || !dst->isTemporary() || dst->refCount());
            if (!m_vm.isSafeToRecurse()) [[unlikely]]
                return emitThrowExpressionTooDeepException();
            if (n->needsDebugHook()) [[unlikely]]
                emitDebugHook(n);
            return n->emitBytecode(*this, dst);
        }

        RegisterID* emitNode(ExpressionNode* n)
        {
            return emitNode(nullptr, n);
        }

        RegisterID* emitNodeInTailPosition(ExpressionNode* n)
        {
            return emitNodeInTailPosition(nullptr, n);
        }

        RegisterID* emitDefineClassElements(PropertyListNode* n, RegisterID* constructor, RegisterID* prototype, Vector<UnlinkedFunctionExecutable::ClassElementDefinition>& instanceElementDefinitions, Vector<UnlinkedFunctionExecutable::ClassElementDefinition>& staticElementDefinitions)
        {
            ASSERT(constructor->refCount() && prototype->refCount());
            if (!m_vm.isSafeToRecurse()) [[unlikely]]
                return emitThrowExpressionTooDeepException();
            if (n->needsDebugHook()) [[unlikely]]
                emitDebugHook(n);
            return n->emitBytecode(*this, constructor, prototype, &instanceElementDefinitions, &staticElementDefinitions);
        }

        RegisterID* emitNodeForProperty(RegisterID* dst, ExpressionNode* node)
        {
            if (node->isString()) {
                if (std::optional<uint32_t> index = parseIndex(static_cast<StringNode*>(node)->value()))
                    return emitLoad(dst, jsNumber(index.value()));
            }
            return emitNode(dst, node);
        }

        RegisterID* emitNodeForProperty(ExpressionNode* n)
        {
            return emitNodeForProperty(nullptr, n);
        }

        void emitNodeInConditionContext(ExpressionNode* n, Label& trueTarget, Label& falseTarget, FallThroughMode fallThroughMode)
        {
            if (!m_vm.isSafeToRecurse()) [[unlikely]] {
                emitThrowExpressionTooDeepException();
                return;
            }
            n->emitBytecodeInConditionContext(*this, trueTarget, falseTarget, fallThroughMode);
        }

        void emitExpressionInfo(const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd)
        {
            ASSERT(divot && divotStart && divotEnd);
            ASSERT(divot.offset >= divotStart.offset);
            ASSERT(divotEnd.offset >= divot.offset);

            // Don't emit expression info if the data could cause us to crash later.
            // In this case we'll just use the wrong info for an error message, not crash.
            if (!divot || !divotStart || !divotEnd)
                return;

            if (m_isBuiltinFunction)
                return;

            unsigned sourceOffset = m_scopeNode->source().startOffset();
            unsigned firstLine = m_scopeNode->source().firstLine().oneBasedInt();

            unsigned divotOffset = divot.offset - sourceOffset;
            unsigned startOffset = divot.offset - divotStart.offset;
            unsigned endOffset = divotEnd.offset - divot.offset;

            unsigned line = divot.line;
            ASSERT(line >= firstLine);
            line -= firstLine;

            unsigned lineStart = divot.lineStartOffset;
            if (lineStart > sourceOffset)
                lineStart -= sourceOffset;
            else
                lineStart = 0;

            if (divotOffset < lineStart)
                return;

            unsigned column = divotOffset - lineStart;

            unsigned instructionOffset = instructions().size();
            m_codeBlock->addExpressionInfo(instructionOffset, divotOffset, startOffset, endOffset, { line, column });
        }


        ALWAYS_INLINE bool leftHandSideNeedsCopy(bool rightHasAssignments, bool rightIsPure)
        {
            return (m_codeType != FunctionCode || rightHasAssignments) && !rightIsPure;
        }

        ALWAYS_INLINE RefPtr<RegisterID> emitNodeForLeftHandSide(ExpressionNode* n, bool rightHasAssignments, bool rightIsPure)
        {
            if (leftHandSideNeedsCopy(rightHasAssignments, rightIsPure)) {
                RefPtr<RegisterID> dst = newTemporary();
                emitNode(dst.get(), n);
                return dst;
            }

            return emitNode(n);
        }

        ALWAYS_INLINE RefPtr<RegisterID> emitNodeForLeftHandSideForProperty(ExpressionNode* n, bool rightHasAssignments, bool rightIsPure)
        {
            if (leftHandSideNeedsCopy(rightHasAssignments, rightIsPure)) {
                RefPtr<RegisterID> dst = newTemporary();
                emitNodeForProperty(dst.get(), n);
                return dst;
            }

            return emitNodeForProperty(n);
        }

        void hoistSloppyModeFunctionIfNecessary(FunctionMetadataNode*);

        ForInContext* findForInContext(RegisterID* property);

    private:
        void emitTypeProfilerExpressionInfo(const JSTextPosition& startDivot, const JSTextPosition& endDivot);
    public:

        // This doesn't emit expression info. If using this, make sure you shouldn't be emitting text offset.
        void emitProfileType(RegisterID* registerToProfile, ProfileTypeBytecodeFlag); 
        // These variables are associated with variables in a program. They could be Locals, ResolvedClosureVar, or ClosureVar.
        void emitProfileType(RegisterID* registerToProfile, const Variable&, const JSTextPosition& startDivot, const JSTextPosition& endDivot);

        void emitProfileType(RegisterID* registerToProfile, ProfileTypeBytecodeFlag, const JSTextPosition& startDivot, const JSTextPosition& endDivot);
        // These are not associated with variables and don't have a global id.
        void emitProfileType(RegisterID* registerToProfile, const JSTextPosition& startDivot, const JSTextPosition& endDivot);

        void emitProfileControlFlow(int);
        
        RegisterID* emitLoadArrowFunctionLexicalEnvironment(const Identifier&);
        RegisterID* ensureThis();
        void emitLoadThisFromArrowFunctionLexicalEnvironment();
        RegisterID* emitLoadNewTargetFromArrowFunctionLexicalEnvironment();

        unsigned addConstantIndex();
        RegisterID* emitLoad(RegisterID* dst, bool);
        RegisterID* emitLoad(RegisterID* dst, const Identifier&);
        RegisterID* emitLoad(RegisterID* dst, JSValue, SourceCodeRepresentation = SourceCodeRepresentation::Other);
        RegisterID* emitLoad(RegisterID* dst, IdentifierSet&& excludedList);

        template<typename UnaryOp, typename = std::enable_if_t<UnaryOp::opcodeID != op_negate>>
        RegisterID* emitUnaryOp(RegisterID* dst, RegisterID* src)
        {
            UnaryOp::emit(this, dst, src);
            return dst;
        }

        RegisterID* emitUnaryOp(OpcodeID, RegisterID* dst, RegisterID* src, ResultType);

        template<typename BinaryOp>
        RegisterID* emitBinaryOp(RegisterID* dst, RegisterID* src1, RegisterID* src2, OperandTypes types = { })
        {
            UNUSED_PARAM(types);
            if constexpr (BinaryOp::opcodeID == op_add || BinaryOp::opcodeID == op_mul || BinaryOp::opcodeID == op_sub || BinaryOp::opcodeID == op_div || BinaryOp::opcodeID == op_bitand || BinaryOp::opcodeID == op_bitor || BinaryOp::opcodeID == op_bitxor)
                BinaryOp::emit(this, dst, src1, src2, m_codeBlock->addBinaryArithProfile(), types);
            else if constexpr (BinaryOp::opcodeID == op_lshift || BinaryOp::opcodeID == op_rshift)
                BinaryOp::emit(this, dst, src1, src2, m_codeBlock->addBinaryArithProfile());
            else
                BinaryOp::emit(this, dst, src1, src2);
            return dst;
        }

        RegisterID* emitBinaryOp(OpcodeID, RegisterID* dst, RegisterID* src1, RegisterID* src2, OperandTypes);

        template<typename EqOp>
        RegisterID* emitEqualityOp(RegisterID* dst, RegisterID* src1, RegisterID* src2)
        {
            static_assert(EqOp::opcodeID == op_eq || EqOp::opcodeID == op_stricteq);
            if (!emitEqualityOpImpl(dst, src1, src2))
                EqOp::emit(this, dst, src1, src2);
            return dst;
        }

        bool emitEqualityOpImpl(RegisterID* dst, RegisterID* src1, RegisterID* src2);

        RegisterID* emitCreateThis(RegisterID* dst);
        RegisterID* emitCreatePromise(RegisterID* dst, RegisterID* newTarget, bool isInternalPromise);
        RegisterID* emitCreateGenerator(RegisterID* dst, RegisterID* newTarget);
        RegisterID* emitCreateAsyncGenerator(RegisterID* dst, RegisterID* newTarget);
        RegisterID* emitInstanceFieldInitializationIfNeeded(RegisterID* dst, RegisterID* constructor, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd);
        void emitTDZCheck(RegisterID* target);
        bool needsTDZCheck(const Variable&);
        void emitTDZCheckIfNecessary(const Variable&, RegisterID* target, RegisterID* scope);
        void liftTDZCheckIfPossible(const Variable&);
        RegisterID* emitNewObject(RegisterID* dst);
        RegisterID* emitNewPromise(RegisterID* dst, bool isInternalPromise);
        RegisterID* emitNewGenerator(RegisterID* dst);
        RegisterID* emitNewArray(RegisterID* dst, ElementNode*, unsigned length, IndexingType recommendedIndexingType); // stops at first elision
        RegisterID* emitNewArrayBuffer(RegisterID* dst, JSImmutableButterfly*, IndexingType recommendedIndexingType);
        // FIXME: new_array_with_spread should use an array allocation profile and take a recommendedIndexingType
        RegisterID* emitNewArrayWithSpread(RegisterID* dst, ElementNode*);
        RegisterID* emitNewArrayWithSize(RegisterID* dst, RegisterID* length);
        RegisterID* emitNewArrayWithSpecies(RegisterID* dst, RegisterID* length, RegisterID* array);

        RegisterID* emitNewFunction(RegisterID* dst, FunctionMetadataNode*);
        RegisterID* emitNewFunctionExpression(RegisterID* dst, FuncExprNode*);
        RegisterID* emitNewDefaultConstructor(RegisterID* dst, ConstructorKind, const Identifier& name, const Identifier& ecmaName, const SourceCode& classSource, NeedsClassFieldInitializer, PrivateBrandRequirement);
        RegisterID* emitNewClassFieldInitializerFunction(RegisterID* dst, Vector<UnlinkedFunctionExecutable::ClassElementDefinition>&&, bool isDerived);
        RegisterID* emitNewArrowFunctionExpression(RegisterID*, ArrowFuncExprNode*);
        RegisterID* emitNewMethodDefinition(RegisterID* dst, MethodDefinitionNode*);
        RegisterID* emitNewRegExp(RegisterID* dst, RegExp*);

        bool shouldSetFunctionName(ExpressionNode*);
        void emitSetFunctionName(RegisterID* value, RegisterID* name);
        void emitSetFunctionName(RegisterID* value, const Identifier&);

        RegisterID* moveLinkTimeConstant(RegisterID* dst, LinkTimeConstant);
        RegisterID* moveEmptyValue(RegisterID* dst);

        RegisterID* emitToNumber(RegisterID* dst, RegisterID* src);
        RegisterID* emitToNumeric(RegisterID* dst, RegisterID* src);
        RegisterID* emitToString(RegisterID* dst, RegisterID* src);
        RegisterID* emitToObject(RegisterID* dst, RegisterID* src, const Identifier& message);
        RegisterID* emitToThis(RegisterID* srcDst);
        RegisterID* emitInc(RegisterID* srcDst);
        RegisterID* emitDec(RegisterID* srcDst);

        RegisterID* emitOverridesHasInstance(RegisterID* dst, RegisterID* constructor, RegisterID* hasInstanceValue);
        RegisterID* emitInstanceof(RegisterID* dst, RegisterID* value, RegisterID* constructor, RegisterID* hasInstanceOrPrototype);
        RegisterID* emitTypeOf(RegisterID* dst, RegisterID* src);
        RegisterID* emitInByVal(RegisterID* dst, RegisterID* property, RegisterID* base);
        RegisterID* emitInById(RegisterID* dst, RegisterID* base, const Identifier& property);

        RegisterID* emitTryGetById(RegisterID* dst, RegisterID* base, const Identifier& property);
        RegisterID* emitGetLength(RegisterID* dst, RegisterID* base);
        RegisterID* emitGetById(RegisterID* dst, RegisterID* base, const Identifier& property);
        RegisterID* emitGetById(RegisterID* dst, RegisterID* base, RegisterID* thisVal, const Identifier& property);
        RegisterID* emitDirectGetById(RegisterID* dst, RegisterID* base, const Identifier& property);
        RegisterID* emitPutById(RegisterID* base, const Identifier& property, RegisterID* value);
        RegisterID* emitPutById(RegisterID* base, RegisterID* thisValue, const Identifier& property, RegisterID* value);
        RegisterID* emitDirectPutById(RegisterID* base, const Identifier& property, RegisterID* value);
        RegisterID* emitDeleteById(RegisterID* dst, RegisterID* base, const Identifier&);
        RegisterID* emitGetByVal(RegisterID* dst, RegisterID* base, RegisterID* property);
        RegisterID* emitGetByVal(RegisterID* dst, RegisterID* base, RegisterID* thisValue, RegisterID* property);
        RegisterID* emitGetPrototypeOf(RegisterID* dst, RegisterID* value);

        template<InvalidPrototypeMode mode>
        RegisterID* emitDirectSetPrototypeOf(RegisterID* base, RegisterID* prototype, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd)
        {
            RefPtr<RegisterID> setPrototypeDirect = moveLinkTimeConstant(nullptr, mode == InvalidPrototypeMode::Throw ? LinkTimeConstant::setPrototypeDirectOrThrow : LinkTimeConstant::setPrototypeDirect);

            CallArguments args(*this, nullptr, 1);
            move(args.thisRegister(), base);
            move(args.argumentRegister(0), prototype);

            emitCallIgnoreResult(newTemporary(), setPrototypeDirect.get(), NoExpectedFunction, args, divot, divotStart, divotEnd, DebuggableCall::No);
            return base;
        }

        RegisterID* emitPutByVal(RegisterID* base, RegisterID* property, RegisterID* value);
        RegisterID* emitPutByVal(RegisterID* base, RegisterID* thisValue, RegisterID* property, RegisterID* value);
        RegisterID* emitPutByValWithECMAMode(RegisterID* base, RegisterID* thisValue, RegisterID* property, RegisterID* value, ECMAMode);
        RegisterID* emitEnumeratorPutByVal(ForInContext&, RegisterID* base, RegisterID* property, RegisterID* value);
        RegisterID* emitDirectPutByVal(RegisterID* base, RegisterID* property, RegisterID* value);
        RegisterID* emitDeleteByVal(RegisterID* dst, RegisterID* base, RegisterID* property);

        RegisterID* emitGetInternalField(RegisterID* dst, RegisterID* base, unsigned index);
        RegisterID* emitPutInternalField(RegisterID* base, unsigned index, RegisterID* value);
        RegisterID* emitDefinePrivateField(RegisterID* base, RegisterID* property, RegisterID* value);
        RegisterID* emitPrivateFieldPut(RegisterID* base, RegisterID* property, RegisterID* value);
        RegisterID* emitGetPrivateName(RegisterID* dst, RegisterID* base, RegisterID* property);
        RegisterID* emitHasPrivateName(RegisterID* dst, RegisterID* base, RegisterID* property);
        RegisterID* emitHasStructureWithFlags(RegisterID* dst, RegisterID* src, unsigned flags);

        void emitCreatePrivateBrand(const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd);
        void emitInstallPrivateBrand(RegisterID* target);
        void emitInstallPrivateClassBrand(RegisterID* target);

        RegisterID* emitGetPrivateBrand(RegisterID* dst, RegisterID* scope, bool isStatic);
        RegisterID* emitHasPrivateBrand(RegisterID* dst, RegisterID* base, RegisterID* brand, bool isStatic);
        void emitCheckPrivateBrand(RegisterID* base, RegisterID* brand, bool isStatic);

        void emitSuperSamplerBegin();
        void emitSuperSamplerEnd();

        RegisterID* emitIdWithProfile(RegisterID* src, SpeculatedType profile);
        void emitUnreachable();

        void emitPutGetterById(RegisterID* base, const Identifier& property, unsigned propertyDescriptorOptions, RegisterID* getter);
        void emitPutSetterById(RegisterID* base, const Identifier& property, unsigned propertyDescriptorOptions, RegisterID* setter);
        void emitPutGetterSetter(RegisterID* base, const Identifier& property, unsigned attributes, RegisterID* getter, RegisterID* setter);
        void emitPutGetterByVal(RegisterID* base, RegisterID* property, unsigned propertyDescriptorOptions, RegisterID* getter);
        void emitPutSetterByVal(RegisterID* base, RegisterID* property, unsigned propertyDescriptorOptions, RegisterID* setter);

        RegisterID* emitGetArgument(RegisterID* dst, int32_t index);

        // Initialize object with generator fields (@generatorThis, @generatorNext, @generatorState, @generatorFrame)
        void emitPutGeneratorFields(RegisterID* nextFunction);
        
        void emitPutAsyncGeneratorFields(RegisterID* nextFunction);

        ExpectedFunction expectedFunctionForIdentifier(const Identifier&);
        RegisterID* emitCall(RegisterID* dst, RegisterID* func, ExpectedFunction, CallArguments&, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);
        RegisterID* emitCallInTailPosition(RegisterID* dst, RegisterID* func, ExpectedFunction, CallArguments&, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);
        RegisterID* emitCallDirectEval(RegisterID* dst, RegisterID* func, CallArguments&, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);
        RegisterID* emitCallVarargs(RegisterID* dst, RegisterID* func, RegisterID* thisRegister, RegisterID* arguments, RegisterID* firstFreeRegister, int32_t firstVarArgOffset, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);
        RegisterID* emitCallVarargsInTailPosition(RegisterID* dst, RegisterID* func, RegisterID* thisRegister, RegisterID* arguments, RegisterID* firstFreeRegister, int32_t firstVarArgOffset, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);
        RegisterID* emitCallForwardArgumentsInTailPosition(RegisterID* dst, RegisterID* func, RegisterID* thisRegister, RegisterID* firstFreeRegister, int32_t firstVarArgOffset, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);

        void emitCallIgnoreResult(RegisterID* dst, RegisterID* func, ExpectedFunction, CallArguments&, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);

        enum PropertyDescriptorOption {
            PropertyConfigurable = 1,
            PropertyWritable     = 1 << 1,
            PropertyEnumerable   = 1 << 2,
        };
        void emitCallDefineProperty(RegisterID* newObj, RegisterID* propertyNameRegister,
            RegisterID* valueRegister, RegisterID* getterRegister, RegisterID* setterRegister, unsigned options, const JSTextPosition&);

        void emitTryWithFinallyThatDoesNotShadowException(const ScopedLambda<void(BytecodeGenerator&)>& emitTry, const ScopedLambda<void(BytecodeGenerator&)>& emitFinally);
        void emitTryWithFinallyThatDoesNotShadowException(FinallyContext&, const ScopedLambda<void(BytecodeGenerator&)>& emitTry, const ScopedLambda<void(BytecodeGenerator&)>& emitFinally);

        void emitGenericEnumeration(ThrowableExpressionData* enumerationNode, ExpressionNode* subjectNode, const ScopedLambda<void(BytecodeGenerator&, RegisterID*)>& callBack, ForOfNode* = nullptr, RegisterID* forLoopSymbolTable = nullptr);
        void emitEnumeration(ThrowableExpressionData* enumerationNode, ExpressionNode* subjectNode, const ScopedLambda<void(BytecodeGenerator&, RegisterID*)>& callBack, ForOfNode* = nullptr, RegisterID* forLoopSymbolTable = nullptr);

        RegisterID* emitGetTemplateObject(RegisterID* dst, TaggedTemplateNode*);
        RegisterID* emitGetGlobalPrivate(RegisterID* dst, const Identifier& property);

        RegisterID* emitReturn(RegisterID* src);
        RegisterID* emitEnd(RegisterID* src);

        RegisterID* emitConstruct(RegisterID* dst, RegisterID* func, RegisterID* lazyThis, ExpectedFunction, CallArguments&, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd);
        RegisterID* emitSuperConstruct(RegisterID* dst, RegisterID* func, RegisterID* lazyThis, ExpectedFunction, CallArguments&, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd);
        RegisterID* emitStrcat(RegisterID* dst, RegisterID* src, int count);
        void emitToPrimitive(RegisterID* dst, RegisterID* src);
        RegisterID* emitToPropertyKey(RegisterID* dst, RegisterID* src);
        RegisterID* emitToPropertyKeyOrNumber(RegisterID* dst, RegisterID* src);

        ResolveType resolveType();
        RegisterID* emitResolveConstantLocal(RegisterID* dst, const Variable&);
        RegisterID* emitResolveScope(RegisterID* dst, const Variable&);
        RegisterID* emitGetFromScope(RegisterID* dst, RegisterID* scope, const Variable&, ResolveMode);
        RegisterID* emitPutToScope(RegisterID* scope, const Variable&, RegisterID* value, ResolveMode, InitializationMode);
        RegisterID* emitPutToScopeDynamic(RegisterID* scope, const Identifier&, RegisterID* value, ResolveMode, InitializationMode);

        RegisterID* emitResolveScopeForHoistingFuncDeclInEval(RegisterID* dst, const Identifier&);

        RegisterID* initializeVariable(const Variable&, RegisterID* value);

        void emitLoopHint();
        void emitJump(Label& target);
        void emitJumpIfTrue(RegisterID* cond, Label& target);
        void emitJumpIfFalse(RegisterID* cond, Label& target);
        void emitJumpIfNotFunctionCall(RegisterID* cond, Label& target);
        void emitJumpIfNotFunctionApply(RegisterID* cond, Label& target);
        void emitJumpIfNotEvalFunction(RegisterID* cond, Label& target);
        void emitJumpIfEmptyPropertyNameEnumerator(RegisterID* cond, Label& target);
        void emitJumpIfSentinelString(RegisterID* cond, Label& target);
        unsigned emitWideJumpIfNotFunctionHasOwnProperty(RegisterID* cond, Label& target);
        void recordHasOwnPropertyInForInLoop(ForInContext&, unsigned branchOffset, Label& genericPath);

        template<typename BinOp, typename JmpOp>
        bool fuseCompareAndJump(RegisterID* cond, Label& target, bool swapOperands = false);

        template<typename UnaryOp, typename JmpOp>
        bool fuseTestAndJmp(RegisterID* cond, Label& target);

        void emitEnter();
        void emitCheckTraps();

        RegisterID* emitGetPropertyEnumerator(RegisterID* dst, RegisterID* base);
        void emitEnumeratorNext(RegisterID* propertyName, RegisterID* mode, RegisterID* index, RegisterID* base, RegisterID* enumerator);
        RegisterID* emitEnumeratorHasOwnProperty(RegisterID* dst, RegisterID* base, RegisterID* mode, RegisterID* propertyName, RegisterID* index, RegisterID* enumerator);

        RegisterID* emitIsCellWithType(RegisterID* dst, RegisterID* src, JSType);
        RegisterID* emitIsGenerator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSGeneratorType); }
        RegisterID* emitIsIteratorHelper(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSIteratorHelperType); }
        RegisterID* emitIsAsyncGenerator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSAsyncGeneratorType); }
        RegisterID* emitIsJSArray(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, ArrayType); }
        RegisterID* emitIsPromise(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSPromiseType); }
        RegisterID* emitIsProxyObject(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, ProxyObjectType); }
        RegisterID* emitIsRegExpObject(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, RegExpObjectType); }
        RegisterID* emitIsMap(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSMapType); }
        RegisterID* emitIsSet(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSSetType); }
        RegisterID* emitIsShadowRealm(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, ShadowRealmType); }
        RegisterID* emitIsStringIterator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSStringIteratorType); }
        RegisterID* emitIsArrayIterator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSArrayIteratorType); }
        RegisterID* emitIsMapIterator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSMapIteratorType); }
        RegisterID* emitIsSetIterator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSSetIteratorType); }
        RegisterID* emitIsWrapForValidIterator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSWrapForValidIteratorType); }
        RegisterID* emitIsRegExpStringIterator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSRegExpStringIteratorType); }
        RegisterID* emitIsObject(RegisterID* dst, RegisterID* src);
        RegisterID* emitIsCallable(RegisterID* dst, RegisterID* src);
        RegisterID* emitIsConstructor(RegisterID* dst, RegisterID* src);
        RegisterID* emitIsNumber(RegisterID* dst, RegisterID* src);
        RegisterID* emitIsNull(RegisterID* dst, RegisterID* src) { return emitEqualityOp<OpStricteq>(dst, src, emitLoad(nullptr, jsNull())); }
        RegisterID* emitIsUndefined(RegisterID* dst, RegisterID* src) { return emitEqualityOp<OpStricteq>(dst, src, emitLoad(nullptr, jsUndefined())); }
        RegisterID* emitIsUndefinedOrNull(RegisterID* dst, RegisterID* src);
        RegisterID* emitIsEmpty(RegisterID* dst, RegisterID* src);
        RegisterID* emitIsDerivedArray(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, DerivedArrayType); }
        RegisterID* emitIsAsyncFromSyncIterator(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, JSAsyncFromSyncIteratorType); }
        RegisterID* emitIsDisposableStack(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, DisposableStackType); }
        RegisterID* emitIsAsyncDisposableStack(RegisterID* dst, RegisterID* src) { return emitIsCellWithType(dst, src, AsyncDisposableStackType); }
        void emitRequireObjectCoercible(RegisterID* value, ASCIILiteral error);

        void emitIteratorOpen(RegisterID* iterator, RegisterID* nextOrIndex, RegisterID* symbolIterator, CallArguments& iterable, const ThrowableExpressionData*);
        void emitIteratorNext(RegisterID* done, RegisterID* value, RegisterID* iterable, RegisterID* nextOrIndex, CallArguments& iterator, const ThrowableExpressionData*);

        RegisterID* emitGetGenericIterator(RegisterID*, ThrowableExpressionData*);
        RegisterID* emitGetAsyncIterator(RegisterID*, ThrowableExpressionData*);

        RegisterID* emitIteratorGenericNext(RegisterID* dst, RegisterID* nextMethod, RegisterID* iterator, const ThrowableExpressionData* node, JSC::EmitAwait = JSC::EmitAwait::No);
        RegisterID* emitIteratorGenericNextWithValue(RegisterID* dst, RegisterID* nextMethod, RegisterID* iterator, RegisterID* value, const ThrowableExpressionData* node);
        void emitIteratorGenericClose(RegisterID* iterator, const ThrowableExpressionData* node, EmitAwait = EmitAwait::No);

        RegisterID* emitRestParameter(RegisterID* result, unsigned numParametersToSkip);

        bool emitReadOnlyExceptionIfNeeded(const Variable&);

        // Start a try block. 'start' must have been emitted.
        TryData* pushTry(Label& start, Label& handlerLabel, HandlerType);
        // End a try block. 'end' must have been emitted.
        void popTry(TryData*, Label& end);

        void emitOutOfLineCatchHandler(RegisterID* thrownValueRegister, RegisterID* completionTypeRegister, TryData*);
        void emitOutOfLineFinallyHandler(RegisterID* exceptionRegister, RegisterID* completionTypeRegister, TryData*);

        void pushClassHeadLexicalScope(VariableEnvironment&);
        void popClassHeadLexicalScope(VariableEnvironment&);

        std::optional<Variable> tryResolveVariable(ExpressionNode*);

    private:
        static constexpr int CurrentLexicalScopeIndex = -2;
        static constexpr int OutermostLexicalScopeIndex = -1;

        int currentLexicalScopeIndex() const
        {
            int size = static_cast<int>(m_lexicalScopeStack.size());
            ASSERT(static_cast<size_t>(size) == m_lexicalScopeStack.size());
            ASSERT(size >= 0);
            if (!size)
                return OutermostLexicalScopeIndex;
            return size - 1;
        }

        void emitOutOfLineExceptionHandler(RegisterID* exceptionRegister, RegisterID* thrownValueRegister, RegisterID* completionTypeRegister, TryData*);

        template<typename ConstructOp>
        RegisterID* emitConstructImpl(RegisterID* dst, RegisterID* func, RegisterID* lazyThis, ExpectedFunction, CallArguments&, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd);

    public:
        enum class ScopeType : uint8_t { CatchScope, CatchScopeWithSimpleParameter, LetConstScope, FunctionNameScope, ClassScope };

        void restoreScopeRegister();
        void restoreScopeRegister(int lexicalScopeIndex);

        int labelScopeDepthToLexicalScopeIndex(int labelScopeDepth);

        void emitThrow(RegisterID*);
        RegisterID* emitArgumentCount(RegisterID*);

        void emitThrowStaticError(ErrorTypeWithExtension, RegisterID*);
        void emitThrowStaticError(ErrorTypeWithExtension, const Identifier& message);
        void emitThrowReferenceError(ASCIILiteral message);
        void emitThrowTypeError(ASCIILiteral message);
        void emitThrowTypeError(const Identifier& message);
        void emitThrowRangeError(const Identifier& message);
        void emitThrowOutOfMemoryError();

        void emitPushCatchScope(VariableEnvironment&, ScopeType);
        void emitPopCatchScope(VariableEnvironment&);

        RegisterID* emitPushWithScope(RegisterID* objectScope);
        void emitPopWithScope();
        void emitPutThisToArrowFunctionContextScope();
        void emitPutNewTargetToArrowFunctionContextScope();
        void emitPutDerivedConstructorToArrowFunctionContextScope();
        RegisterID* emitLoadDerivedConstructorFromArrowFunctionLexicalEnvironment();
        RegisterID* emitLoadDerivedConstructor();

        void emitDebugHook(DebugHookType, const JSTextPosition&, RegisterID* data = nullptr);
        void emitDebugHook(StatementNode*, RegisterID* data = nullptr);
        void emitDebugHook(ExpressionNode*, RegisterID* data = nullptr);
        void emitWillLeaveCallFrameDebugHook();

        RegisterID* emitLoad(RegisterID* dst, CompletionType type) { return emitLoad(dst, jsNumber(static_cast<int32_t>(type))); }
        RegisterID* emitLoad(RegisterID* dst, JSGenerator::ResumeMode mode) { return emitLoad(dst, jsNumber(static_cast<int32_t>(mode))); }

        bool emitJumpViaFinallyIfNeeded(int targetLabelScopeDepth, Label& jumpTarget);
        bool emitReturnViaFinallyIfNeeded(RegisterID* returnRegister);
        void emitFinallyCompletion(FinallyContext&, Label& normalCompletionLabel);

    public:
        void pushFinallyControlFlowScope(FinallyContext&);
        void popFinallyControlFlowScope();

        void pushOptionalChainTarget();
        void popOptionalChainTarget();
        void popOptionalChainTarget(RegisterID* dst, bool isDelete);
        void emitOptionalCheck(RegisterID* src);

        void pushForInScope(RegisterID* local, RegisterID* propertyName, RegisterID* propertyOffset, RegisterID* enumerator, RegisterID* mode, std::optional<Variable> base);
        void popForInScope(RegisterID* local);

        LabelScope* breakTarget(const Identifier&);
        LabelScope* continueTarget(const Identifier&);

        void beginSwitch(RegisterID*, SwitchInfo::SwitchType);
        void endSwitch(const Vector<Ref<Label>, 8>&, ExpressionNode**, Label& defaultLabel, int32_t min, int32_t range);

        void emitYieldPoint(RegisterID*, JSAsyncGenerator::AsyncGeneratorSuspendReason);

        void emitGeneratorStateLabel();
        void emitGeneratorStateChange(int32_t state);
        RegisterID* emitYield(RegisterID* argument);
        RegisterID* emitAwait(RegisterID* dst, RegisterID* src, const JSTextPosition&);
        RegisterID* emitDelegateYield(RegisterID* argument, ThrowableExpressionData*);
        RegisterID* generatorStateRegister() { return &m_parameters[static_cast<int32_t>(JSGenerator::Argument::State)]; }
        RegisterID* generatorValueRegister() { return &m_parameters[static_cast<int32_t>(JSGenerator::Argument::Value)]; }
        RegisterID* generatorResumeModeRegister() { return &m_parameters[static_cast<int32_t>(JSGenerator::Argument::ResumeMode)]; }
        RegisterID* generatorFrameRegister() { return &m_parameters[static_cast<int32_t>(JSGenerator::Argument::Frame)]; }

        CodeType codeType() const { return m_codeType; }

        bool shouldBeConcernedWithCompletionValue() const { return !m_defaultAllowCallIgnoreResultOptimization; }

        bool shouldEmitDebugHooks() const { return m_codeGenerationMode.contains(CodeGenerationMode::Debugger) && !m_isBuiltinFunction; }
        bool shouldEmitTypeProfilerHooks() const { return m_codeGenerationMode.contains(CodeGenerationMode::TypeProfiler); }
        bool shouldEmitControlFlowProfilerHooks() const { return m_codeGenerationMode.contains(CodeGenerationMode::ControlFlowProfiler); }
        
        ECMAMode ecmaMode() const { return m_ecmaMode; }
        void setUsesCheckpoints() { m_codeBlock->setHasCheckpoints(); }

        SourceParseMode parseMode() const { return m_codeBlock->parseMode(); }
        
        bool isBuiltinFunction() const { return m_isBuiltinFunction; }

        OpcodeID lastOpcodeID() const { return m_lastOpcodeID; }
        
        bool isDerivedConstructorContext() { return m_derivedContextType == DerivedContextType::DerivedConstructorContext; }
        bool isDerivedClassContext() { return m_derivedContextType == DerivedContextType::DerivedMethodContext; }
        bool isArrowFunction() { return m_codeBlock->isArrowFunction(); }

        enum class TDZCheckOptimization { Optimize, DoNotOptimize };
        enum class NestedScopeType { IsNested, IsNotNested };
    private:
        enum class TDZRequirement { UnderTDZ, NotUnderTDZ };
        enum class ScopeRegisterType { Var, Block };
        void pushLexicalScopeInternal(VariableEnvironment&, TDZCheckOptimization, NestedScopeType, RegisterID** constantSymbolTableResult, TDZRequirement, ScopeType, ScopeRegisterType);
        void initializeBlockScopedFunctions(VariableEnvironment&, FunctionStack&, RegisterID* constantSymbolTable);
        void popLexicalScopeInternal(VariableEnvironment&);
        template<typename LookUpVarKindFunctor>
        bool instantiateLexicalVariables(const VariableEnvironment&, ScopeType, SymbolTable*, ScopeRegisterType, LookUpVarKindFunctor);
        void emitPrefillStackTDZVariables(const VariableEnvironment&, SymbolTable*);
        RegisterID* emitGetParentScope(RegisterID* dst, RegisterID* scope);
        void emitPushFunctionNameScope(const Identifier& property, RegisterID* value, bool isCaptured);
        void emitNewFunctionExpressionCommon(RegisterID*, FunctionMetadataNode*);
        
        bool isNewTargetUsedInInnerArrowFunction();
        bool isArgumentsUsedInInnerArrowFunction();

        void emitToThis() { emitToThis(&m_thisRegister); }

        RegisterID* emitMove(RegisterID* dst, RegisterID* src);

    public:
        void disablePeepholeOptimization() { m_lastOpcodeID = op_end; }
    private:
        bool canDoPeepholeOptimization() const { return m_lastOpcodeID != op_end; }

    public:
        bool isSuperUsedInInnerArrowFunction();
        bool isSuperCallUsedInInnerArrowFunction();
        bool isThisUsedInInnerArrowFunction();
        void pushLexicalScope(VariableEnvironmentNode*, ScopeType, TDZCheckOptimization, NestedScopeType = NestedScopeType::IsNotNested, RegisterID** constantSymbolTableResult = nullptr, bool shouldInitializeBlockScopedFunctions = true);
        void pushClassLexicalScope(VariableEnvironmentNode*);
        void popLexicalScope(VariableEnvironmentNode*);
        void prepareLexicalScopeForNextForLoopIteration(VariableEnvironmentNode*, RegisterID* loopSymbolTable);
        int labelScopeDepth() const;

    private:
        ParserError generate(unsigned&);
        Variable variableForLocalEntry(const Identifier&, const SymbolTableEntry&, int symbolTableConstantIndex, bool isLexicallyScoped);

        RegisterID* kill(RegisterID* dst)
        {
            m_staticPropertyAnalyzer.kill(dst);
            return dst;
        }

        void retrieveLastUnaryOp(int& dstIndex, int& srcIndex);
        ALWAYS_INLINE void rewind();

        void allocateScope();

        template<typename JumpOp>
        void setTargetForJumpInstruction(JSInstructionStream::MutableRef&, int target);

        using BigIntMapEntry = std::tuple<UniquedStringImpl*, uint8_t, bool>;

        using NumberMap = UncheckedKeyHashMap<double, JSValue>;
        using IdentifierStringMap = UncheckedKeyHashMap<UniquedStringImpl*, JSString*, IdentifierRepHash>;
        using IdentifierBigIntMap = UncheckedKeyHashMap<BigIntMapEntry, JSValue>;
        using TemplateObjectDescriptorSet = UncheckedKeyHashSet<Ref<TemplateObjectDescriptor>>;
        using TemplateDescriptorMap = UncheckedKeyHashMap<uint64_t, JSTemplateObjectDescriptor*, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

        // Helper for emitCall() and emitConstruct(). This works because the set of
        // expected functions have identical behavior for both call and construct
        // (i.e. "Object()" is identical to "new Object()").
        ExpectedFunction emitExpectedFunctionSnippet(RegisterID* dst, RegisterID* func, ExpectedFunction, CallArguments&, Label& done);

        LexicallyScopedFeatures computeFeaturesForCallDirectEval();
        
        template<typename CallOp>
        RegisterID* emitCall(RegisterID* dst, RegisterID* func, ExpectedFunction, CallArguments&, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);

        RegisterID* emitCallIterator(RegisterID* iterator, RegisterID* argument, ThrowableExpressionData*);

        // Initializes the stack form the parameter; does nothing for the symbol table.
        RegisterID* initializeNextParameter();
        UniquedStringImpl* visibleNameForParameter(DestructuringPatternNode*);
        
        RegisterID& registerFor(VirtualRegister reg)
        {
            if (reg.isLocal())
                return m_calleeLocals[reg.toLocal()];

            if (reg.offset() == CallFrameSlot::callee)
                return m_calleeRegister;

            ASSERT(m_parameters.size());
            return m_parameters[reg.toArgument()];
        }

        bool hasConstant(const Identifier&) const;
        unsigned addConstant(const Identifier&);
        RegisterID* addConstantValue(JSValue, SourceCodeRepresentation = SourceCodeRepresentation::Other);
        RegisterID* addConstantEmptyValue();

        UnlinkedFunctionExecutable* makeFunction(FunctionMetadataNode* metadata)
        {
            DerivedContextType newDerivedContextType = DerivedContextType::None;

            NeedsClassFieldInitializer needsClassFieldInitializer = metadata->isConstructorAndNeedsClassFieldInitializer() ? NeedsClassFieldInitializer::Yes : NeedsClassFieldInitializer::No;
            PrivateBrandRequirement privateBrandRequirement = metadata->privateBrandRequirement();
            if (SourceParseModeSet(SourceParseMode::ArrowFunctionMode, SourceParseMode::AsyncArrowFunctionMode, SourceParseMode::AsyncArrowFunctionBodyMode).contains(metadata->parseMode())) {
                if (constructorKind() == ConstructorKind::Extends || isDerivedConstructorContext()) {
                    newDerivedContextType = DerivedContextType::DerivedConstructorContext;
                    needsClassFieldInitializer = m_codeBlock->needsClassFieldInitializer();
                    privateBrandRequirement = m_codeBlock->privateBrandRequirement();
                }
                else if (m_codeBlock->isClassContext() || isDerivedClassContext())
                    newDerivedContextType = DerivedContextType::DerivedMethodContext;
            }

            auto optionalVariablesUnderTDZ = getVariablesUnderTDZ();
            std::optional<Vector<Identifier>> generatorOrAsyncWrapperFunctionParameterNames = std::nullopt;
            std::optional<PrivateNameEnvironment> parentPrivateNameEnvironment = getAvailablePrivateAccessNames();

            // FIXME: These flags, ParserModes and propagation to XXXCodeBlocks should be reorganized.
            // https://bugs.webkit.org/show_bug.cgi?id=151547
            SourceParseMode parseMode = metadata->parseMode();
            ConstructAbility constructAbility = constructAbilityForParseMode(parseMode);
            if (parseMode == SourceParseMode::MethodMode && metadata->constructorKind() != ConstructorKind::None)
                constructAbility = ConstructAbility::CanConstruct;

            if (isGeneratorOrAsyncFunctionWrapperParseMode(m_codeBlock->parseMode()) && isGeneratorOrAsyncFunctionBodyParseMode(parseMode))
                generatorOrAsyncWrapperFunctionParameterNames = getParameterNames();

            return UnlinkedFunctionExecutable::create(m_vm, m_scopeNode->source(), metadata, isBuiltinFunction() ? UnlinkedBuiltinFunction : UnlinkedNormalFunction, constructAbility, InlineAttribute::None, scriptMode(), WTFMove(optionalVariablesUnderTDZ), WTFMove(generatorOrAsyncWrapperFunctionParameterNames), WTFMove(parentPrivateNameEnvironment), newDerivedContextType, needsClassFieldInitializer, privateBrandRequirement);
        }

        RefPtr<TDZEnvironmentLink> getVariablesUnderTDZ();
        Vector<Identifier> getParameterNames() const;
        std::optional<PrivateNameEnvironment> getAvailablePrivateAccessNames();

        RegisterID* emitConstructVarargs(RegisterID* dst, RegisterID* func, RegisterID* thisRegister, RegisterID* arguments, RegisterID* firstFreeRegister, int32_t firstVarArgOffset, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);
        RegisterID* emitSuperConstructVarargs(RegisterID* dst, RegisterID* func, RegisterID* thisRegister, RegisterID* arguments, RegisterID* firstFreeRegister, int32_t firstVarArgOffset, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);

        template<typename CallOp>
        RegisterID* emitCallVarargs(RegisterID* dst, RegisterID* func, RegisterID* thisRegister, RegisterID* arguments, RegisterID* firstFreeRegister, int32_t firstVarArgOffset, const JSTextPosition& divot, const JSTextPosition& divotStart, const JSTextPosition& divotEnd, DebuggableCall);
        
        void emitLogShadowChickenPrologueIfNecessary();
        void emitLogShadowChickenTailIfNecessary();

        void initializeParameters(FunctionParameters&);
        void initializeVarLexicalEnvironment(int symbolTableConstantIndex, SymbolTable* functionSymbolTable, bool hasCapturedVariables);
        void initializeDefaultParameterValuesAndSetupFunctionScopeStack(FunctionParameters&, bool isSimpleParameterList, FunctionNode*, SymbolTable*, int symbolTableConstantIndex, const ScopedLambda<bool (UniquedStringImpl*)>& captures, bool shouldCreateArgumentsVariableInParameterScope);
        void initializeArrowFunctionContextScopeIfNeeded(SymbolTable* functionSymbolTable = nullptr, bool canReuseLexicalEnvironment = false);
        bool needsDerivedConstructorInArrowFunctionLexicalEnvironment();

        enum class TDZNecessityLevel {
            NotNeeded,
            Optimize,
            DoNotOptimize
        };
        typedef UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, TDZNecessityLevel, IdentifierRepHash> TDZMap;

    public:
        JSString* addStringConstant(const Identifier&);
        JSValue addBigIntConstant(const Identifier&, uint8_t radix, bool sign);
        RegisterID* addTemplateObjectConstant(Ref<TemplateObjectDescriptor>&&, int);

        const JSInstructionStreamWriter& instructions() const { return m_writer; }

        RegisterID* emitThrowExpressionTooDeepException();

        using TDZStackEntry = std::pair<TDZMap, RefPtr<TDZEnvironmentLink>>;

        class PreservedTDZStack {
        private:
            Vector<TDZStackEntry> m_preservedTDZStack;
            friend class BytecodeGenerator;
        };

        void preserveTDZStack(PreservedTDZStack&);
        void restoreTDZStack(const PreservedTDZStack&);

        template<typename Func>
        void withWriter(JSInstructionStreamWriter& writer, const Func& fn)
        {
            auto prevLastOpcodeID = m_lastOpcodeID;
            auto prevLastInstruction = m_lastInstruction;
            m_writer.swap(writer);
            disablePeepholeOptimization();
            m_lastInstruction = m_writer.ref();
            fn();
            m_writer.swap(writer);
            m_lastOpcodeID = prevLastOpcodeID;
            m_lastInstruction = prevLastInstruction;
        }

        PrivateNameEntry getPrivateTraits(const Identifier&);

        void pushPrivateAccessNames(const PrivateNameEnvironment*);
        void popPrivateAccessNames();

        bool needsArguments() const { return m_needsArguments; };
        bool shouldGetArgumentsDotLengthFast(ExpressionNode* node) const
        {
            return isFunctionNode()
                && !needsArguments()
                && !hasShadowsArgumentsCodeFeature()
                && node->isArgumentsLengthAccess(vm())
                && !isArrowFunctionParseMode(parseMode())
                && !isGeneratorOrAsyncFunctionBodyParseMode(parseMode());
        }

        unsigned localScopeCount() const { return m_localScopeCount; }
    private:
        OptionSet<CodeGenerationMode> m_codeGenerationMode;

        struct LexicalScopeStackEntry {
            SymbolTable* m_symbolTable;
            RegisterID* m_scope;
            bool m_isWithScope;
            int m_symbolTableConstantIndex;
        };
        Vector<LexicalScopeStackEntry> m_lexicalScopeStack;

        RefPtr<TDZEnvironmentLink> m_cachedParentTDZ;
        const FixedVector<Identifier>* m_generatorOrAsyncWrapperFunctionParameterNames { nullptr };
        Vector<TDZStackEntry> m_TDZStack;
        Vector<PrivateNameEnvironment> m_privateNamesStack;
        std::optional<size_t> m_varScopeLexicalScopeStackIndex;
        void pushTDZVariables(const VariableEnvironment&, TDZCheckOptimization, TDZRequirement);

        ScopeNode* const m_scopeNode;

        // Some of these objects keep pointers to one another. They are arranged
        // to ensure a sane destruction order that avoids references to freed memory.
        UncheckedKeyHashSet<RefPtr<UniquedStringImpl>, IdentifierRepHash> m_functions;
        RegisterID m_ignoredResultRegister;
        RegisterID m_thisRegister;
        RegisterID m_calleeRegister;
        RegisterID* m_scopeRegister { nullptr };
        RegisterID* m_topLevelScopeRegister { nullptr }; // This may not be set. Only Eval and Module will configure this register.
        RegisterID* m_argumentsRegister { nullptr };
        RegisterID* m_lexicalEnvironmentRegister { nullptr };
        RegisterID* m_generatorRegister { nullptr };
        RegisterID* m_emptyValueRegister { nullptr };
        RegisterID* m_newTargetRegister { nullptr };
        RegisterID* m_isDerivedConstuctor { nullptr };
        UncheckedKeyHashMap<LinkTimeConstant, RegisterID*, WTF::IntHash<LinkTimeConstant>, WTF::StrongEnumHashTraits<LinkTimeConstant>> m_linkTimeConstantRegisters;
        RegisterID* m_arrowFunctionContextLexicalEnvironmentRegister { nullptr };
        RegisterID* m_promiseRegister { nullptr };

        FinallyContext* m_currentFinallyContext { nullptr };

        SegmentedVector<RegisterID, 32> m_parameters;
        SegmentedVector<LabelScope, 32> m_labelScopes;
        SegmentedVector<RegisterID, 32> m_constantPoolRegisters;
        unsigned m_finallyDepth { 0 };
        unsigned m_localScopeDepth { 0 };
        unsigned m_localScopeCount { 0 };
        const CodeType m_codeType;

        unsigned localScopeDepth() const;
        void pushLocalControlFlowScope();
        void popLocalControlFlowScope();

        // FIXME: Restore overflow checking with UnsafeVectorOverflow once SegmentVector supports it.
        // https://bugs.webkit.org/show_bug.cgi?id=165980
        SegmentedVector<ControlFlowScope, 16> m_controlFlowScopeStack;
        Vector<SwitchInfo> m_switchContextStack;
        Vector<Ref<ForInContext>> m_forInContextStack;
        Vector<TryContext> m_tryContextStack;
        unsigned m_yieldPoints { 0 };
        bool m_needsGeneratorification { false };

        Strong<SymbolTable> m_generatorFrameSymbolTable;
        int m_generatorFrameSymbolTableIndex { 0 };

        enum FunctionVariableType : uint8_t { NormalFunctionVariable, TopLevelFunctionVariable };
        Vector<std::pair<FunctionMetadataNode*, FunctionVariableType>> m_functionsToInitialize;
        bool m_needToInitializeArguments { false };
        RestParameterNode* m_restParameter { nullptr };

        struct AsyncFuncParametersTryCatchInfo {
            RefPtr<Label> catchStartLabel { nullptr };
            RefPtr<RegisterID> thrownValue { nullptr };
        }; 
        std::optional<AsyncFuncParametersTryCatchInfo> m_asyncFuncParametersTryCatchInfo;

        template<typename EmitBytecodeFunctor>
        void asyncFuncParametersTryCatchWrap(const EmitBytecodeFunctor&);

        Vector<TryRange> m_tryRanges;
        SegmentedVector<TryData, 8> m_tryData;

        Vector<Ref<Label>> m_optionalChainTargetStack;

        int m_nextConstantOffset { 0 };
        
        // Constant pool
        IdentifierMap m_identifierMap;

        typedef UncheckedKeyHashMap<EncodedJSValueWithRepresentation, unsigned, EncodedJSValueWithRepresentationHash, EncodedJSValueWithRepresentationHashTraits> JSValueMap;
        JSValueMap m_jsValueMap;
        IdentifierStringMap m_stringMap;
        IdentifierBigIntMap m_bigIntMap;
        TemplateObjectDescriptorSet m_templateObjectDescriptorSet;
        TemplateDescriptorMap m_templateDescriptorMap;

        StaticPropertyAnalyzer m_staticPropertyAnalyzer;

        VM& m_vm;

        bool m_defaultAllowCallIgnoreResultOptimization { false };
        bool m_usesExceptions { false };
        bool m_expressionTooDeep { false };
        bool m_isBuiltinFunction { false };
        bool m_usesSloppyEval { false };
        bool m_allowTailCallOptimization { false };
        bool m_allowCallIgnoreResultOptimization { false };
        bool m_needsToUpdateArrowFunctionContext : 1;
        bool m_needsArguments : 1 { false };
        ECMAMode m_ecmaMode;
        DerivedContextType m_derivedContextType { DerivedContextType::None };

        struct CatchEntry {
            TryData* tryData;
            VirtualRegister exceptionRegister;
            VirtualRegister thrownValueRegister;
            VirtualRegister completionTypeRegister;
        };
        Vector<CatchEntry> m_exceptionHandlersToEmit;

        struct {
            JSTextPosition position;
            DebugHookType type { DidExecuteProgram };
        } m_lastDebugHook;
    };

    class StrictModeScope : private SetForScope<ECMAMode> {
    public:
        StrictModeScope(BytecodeGenerator& generator)
            : SetForScope(generator.m_ecmaMode, ECMAMode::strict())
        {
        }
    };

} // namespace JSC

namespace WTF {

void printInternal(PrintStream&, JSC::Variable::VariableKind);

} // namespace WTF

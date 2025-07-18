/*
*  Copyright (C) 1999-2002 Harri Porten (porten@kde.org)
*  Copyright (C) 2001 Peter Kelly (pmk@post.com)
*  Copyright (C) 2003-2022 Apple Inc. All rights reserved.
*  Copyright (C) 2007 Cameron Zwarich (cwzwarich@uwaterloo.ca)
*  Copyright (C) 2007 Maks Orlovich
*  Copyright (C) 2007 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2012 Igalia, S.L.
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
#include "Nodes.h"
#include "NodeConstructors.h"

#include "AbstractModuleRecord.h"
#include "BuiltinNames.h"
#include "BytecodeGenerator.h"
#include "BytecodeGeneratorBaseInlines.h"
#include "JSArrayIterator.h"
#include "JSAsyncDisposableStack.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSDisposableStack.h"
#include "JSGenerator.h"
#include "JSImmutableButterfly.h"
#include "JSIteratorHelper.h"
#include "JSMapIterator.h"
#include "JSPromise.h"
#include "JSRegExpStringIterator.h"
#include "JSSetIterator.h"
#include "JSStringIterator.h"
#include "JSWrapForValidIterator.h"
#include "LabelScope.h"
#include "LinkTimeConstant.h"
#include "ModuleScopeData.h"
#include "StackAlignment.h"
#include "UnlinkedMetadataTableInlines.h"
#include "YarrFlags.h"
#include <wtf/Assertions.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {

/*
    Details of the emitBytecode function.

    Return value: The register holding the production's value.
             dst: An optional parameter specifying the most efficient destination at
                  which to store the production's value.
                  If dst is null, you may return whatever VirtualRegister you want. Otherwise you have to return dst.

    The dst argument provides for a crude form of copy propagation. For example,

        x = 1

    becomes

        load r[x], 1

    instead of 

        load r0, 1
        mov r[x], r0

    because the assignment node, "x =", passes r[x] as dst to the number node, "1".
*/

void ExpressionNode::emitBytecodeInConditionContext(BytecodeGenerator& generator, Label& trueTarget, Label& falseTarget, FallThroughMode fallThroughMode)
{
    RegisterID* result = generator.emitNode(this);
    if (fallThroughMode == FallThroughMeansTrue)
        generator.emitJumpIfFalse(result, falseTarget);
    else
        generator.emitJumpIfTrue(result, trueTarget);
}

// ------------------------------ ThrowableExpressionData --------------------------------

RegisterID* ThrowableExpressionData::emitThrowReferenceError(BytecodeGenerator& generator, ASCIILiteral message, RegisterID* dst)
{
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    generator.emitThrowReferenceError(message);
    if (dst)
        return dst;
    return generator.newTemporary();
}

// ------------------------------ ConstantNode ----------------------------------

void ConstantNode::emitBytecodeInConditionContext(BytecodeGenerator& generator, Label& trueTarget, Label& falseTarget, FallThroughMode fallThroughMode)
{
    TriState value = TriState::Indeterminate;
    JSValue constant = jsValue(generator);
    if (constant) [[likely]]
        value = constant.pureToBoolean();

    if (needsDebugHook()) [[unlikely]] {
        if (value != TriState::Indeterminate)
            generator.emitDebugHook(this);
    }

    if (value == TriState::Indeterminate)
        ExpressionNode::emitBytecodeInConditionContext(generator, trueTarget, falseTarget, fallThroughMode);
    else if (value == TriState::True && fallThroughMode == FallThroughMeansFalse)
        generator.emitJump(trueTarget);
    else if (value == TriState::False && fallThroughMode == FallThroughMeansTrue)
        generator.emitJump(falseTarget);

    // All other cases are unconditional fall-throughs, like "if (true)".
}

RegisterID* ConstantNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        return nullptr;
    JSValue constant = jsValue(generator);
    if (!constant) [[unlikely]] {
        // This can happen if we try to parse a string or BigInt so enormous that we OOM.
        return generator.emitThrowExpressionTooDeepException();
    }
    return generator.emitLoad(dst, constant);
}

JSValue StringNode::jsValue(BytecodeGenerator& generator) const
{
    return generator.addStringConstant(m_value);
}

JSValue BigIntNode::jsValue(BytecodeGenerator& generator) const
{
    return generator.addBigIntConstant(m_value, m_radix, m_sign);
}

// ------------------------------ NumberNode ----------------------------------

RegisterID* NumberNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        return nullptr;
    return generator.emitLoad(dst, jsValue(generator), isIntegerNode() ? SourceCodeRepresentation::Integer : SourceCodeRepresentation::Double);
}

// ------------------------------ RegExpNode -----------------------------------

RegisterID* RegExpNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        return nullptr;

    auto flags = Yarr::parseFlags(m_flags.string());
    ASSERT(flags);
    RegExp* regExp = RegExp::create(generator.vm(), m_pattern.string(), flags.value());
    if (regExp->isValid())
        return generator.emitNewRegExp(generator.finalDestination(dst), regExp);

    auto& message = generator.parserArena().identifierArena().makeIdentifier(generator.vm(), regExp->errorMessage().span8());
    generator.emitThrowStaticError(ErrorTypeWithExtension::SyntaxError, message);
    return generator.emitLoad(generator.finalDestination(dst), jsUndefined());
}

// ------------------------------ ThisNode -------------------------------------

RegisterID* ThisNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    generator.ensureThis();
    if (dst == generator.ignoredResult())
        return nullptr;

    RegisterID* result = generator.move(dst, generator.thisRegister());
    static const unsigned thisLength = strlen("this");
    generator.emitProfileType(generator.thisRegister(), position(), position() + thisLength);
    return result;
}

// ------------------------------ SuperNode -------------------------------------

static RegisterID* emitHomeObjectForCallee(BytecodeGenerator& generator)
{
    if ((generator.isDerivedClassContext() || generator.isDerivedConstructorContext()) && generator.parseMode() != SourceParseMode::ClassFieldInitializerMode) {
        RegisterID* derivedConstructor = generator.emitLoadDerivedConstructorFromArrowFunctionLexicalEnvironment();
        return generator.emitGetById(generator.newTemporary(), derivedConstructor, generator.propertyNames().builtinNames().homeObjectPrivateName());
    }

    RegisterID callee;
    callee.setIndex(CallFrameSlot::callee);
    return generator.emitGetById(generator.newTemporary(), &callee, generator.propertyNames().builtinNames().homeObjectPrivateName());
}

static RegisterID* emitSuperBaseForCallee(BytecodeGenerator& generator)
{
    RefPtr<RegisterID> homeObject = emitHomeObjectForCallee(generator);
    return generator.emitGetPrototypeOf(generator.newTemporary(), homeObject.get());
}

static RegisterID* emitGetSuperFunctionForConstruct(BytecodeGenerator& generator)
{
    if (generator.isDerivedConstructorContext())
        return generator.emitGetPrototypeOf(generator.newTemporary(), generator.emitLoadDerivedConstructorFromArrowFunctionLexicalEnvironment());

    RegisterID callee;
    callee.setIndex(CallFrameSlot::callee);
    return generator.emitGetPrototypeOf(generator.newTemporary(), &callee);
}

RegisterID* SuperNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RegisterID* result = emitSuperBaseForCallee(generator);
    return generator.move(generator.finalDestination(dst), result);
}

// ------------------------------ ImportNode -------------------------------------

RegisterID* ImportNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> importModule = generator.moveLinkTimeConstant(nullptr, LinkTimeConstant::importModule);
    CallArguments arguments(generator, nullptr, m_option ? 2 : 1);
    generator.emitLoad(arguments.thisRegister(), jsUndefined());
    generator.emitNode(arguments.argumentRegister(0), m_expr);
    if (m_option)
        generator.emitNode(arguments.argumentRegister(1), m_option);
    return generator.emitCall(generator.finalDestination(dst, importModule.get()), importModule.get(), NoExpectedFunction, arguments, divot(), divotStart(), divotEnd(), DebuggableCall::No);
}

// ------------------------------ NewTargetNode ----------------------------------

RegisterID* NewTargetNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        return nullptr;

    return generator.move(dst, generator.newTarget());
}

// ------------------------------ ImportMetaNode ---------------------------------

RegisterID* ImportMetaNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    return generator.emitNode(dst, m_expr);
}

// ------------------------------ ResolveNode ----------------------------------

bool ResolveNode::isPure(BytecodeGenerator& generator) const
{
    return generator.variable(m_ident).offset().isStack();
}

bool ResolveNode::getFromScopeCanThrow(BytecodeGenerator& generator) const
{
    Variable var = generator.variable(m_ident);
    if (var.offset().isStack() || var.offset().isScope())
        return !generator.needsTDZCheck(var);

    return true;
}

RegisterID* ResolveNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    Variable var = generator.variable(m_ident);
    if (RegisterID* local = var.local()) {
        generator.emitTDZCheckIfNecessary(var, local, nullptr);
        if (dst == generator.ignoredResult())
            return nullptr;

        generator.emitProfileType(local, var, m_position, m_position + m_ident.length());
        return generator.move(dst, local);
    }
    
    JSTextPosition divot = m_start + m_ident.length();
    generator.emitExpressionInfo(divot, m_start, divot);
    RefPtr<RegisterID> scope = generator.emitResolveScope(dst, var);
    RegisterID* finalDest = generator.finalDestination(dst);
    if (!generator.needsTDZCheck(var))
        generator.emitGetFromScope(finalDest, scope.get(), var, ThrowIfNotFound);
    else {
        RefPtr<RegisterID> uncheckedResult = generator.newTemporary();
        generator.emitGetFromScope(uncheckedResult.get(), scope.get(), var, ThrowIfNotFound);
        generator.emitTDZCheck(uncheckedResult.get());
        generator.move(finalDest, uncheckedResult.get());
    }
    generator.emitProfileType(finalDest, var, m_position, m_position + m_ident.length());
    return finalDest;
}

// ------------------------------ TemplateStringNode -----------------------------------

RegisterID* TemplateStringNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        return nullptr;
    ASSERT(cooked());
    return generator.emitLoad(dst, JSValue(generator.addStringConstant(*cooked())));
}

// ------------------------------ TemplateLiteralNode -----------------------------------

RegisterID* TemplateLiteralNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (!m_templateExpressions) {
        TemplateStringNode* templateString = m_templateStrings->value();
        ASSERT_WITH_MESSAGE(!m_templateStrings->next(), "Only one template element exists because there's no expression in a given template literal.");
        return generator.emitNode(dst, templateString);
    }

    Vector<RefPtr<RegisterID>, 16> temporaryRegisters;

    TemplateStringListNode* templateString = m_templateStrings;
    TemplateExpressionListNode* templateExpression = m_templateExpressions;
    for (; templateExpression; templateExpression = templateExpression->next(), templateString = templateString->next()) {
        // Evaluate TemplateString.
        ASSERT(templateString->value()->cooked());
        if (!templateString->value()->cooked()->isEmpty()) {
            temporaryRegisters.append(generator.newTemporary());
            generator.emitNode(temporaryRegisters.last().get(), templateString->value());
        }

        // Evaluate Expression.
        temporaryRegisters.append(generator.newTemporary());
        generator.emitNode(temporaryRegisters.last().get(), templateExpression->value());
        generator.emitToString(temporaryRegisters.last().get(), temporaryRegisters.last().get());
    }

    // Evaluate tail TemplateString.
    ASSERT(templateString->value()->cooked());
    if (!templateString->value()->cooked()->isEmpty()) {
        temporaryRegisters.append(generator.newTemporary());
        generator.emitNode(temporaryRegisters.last().get(), templateString->value());
    }

    if (temporaryRegisters.size() == 1)
        return generator.emitToString(generator.finalDestination(dst, temporaryRegisters[0].get()), temporaryRegisters[0].get());

    return generator.emitStrcat(generator.finalDestination(dst, temporaryRegisters[0].get()), temporaryRegisters[0].get(), temporaryRegisters.size());
}

// ------------------------------ TaggedTemplateNode -----------------------------------

RegisterID* TaggedTemplateNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ExpectedFunction expectedFunction = NoExpectedFunction;
    RefPtr<RegisterID> tag = nullptr;
    RefPtr<RegisterID> base = nullptr;
    if (!m_tag->isLocation()) {
        tag = generator.newTemporary();
        tag = generator.emitNode(tag.get(), m_tag);
    } else if (m_tag->isResolveNode()) {
        ResolveNode* resolve = static_cast<ResolveNode*>(m_tag);
        const Identifier& identifier = resolve->identifier();
        expectedFunction = generator.expectedFunctionForIdentifier(identifier);

        Variable var = generator.variable(identifier);
        if (RegisterID* local = var.local()) {
            generator.emitTDZCheckIfNecessary(var, local, nullptr);
            tag = generator.move(generator.newTemporary(), local);
        } else {
            tag = generator.newTemporary();
            base = generator.newTemporary();

            JSTextPosition newDivot = divotStart() + identifier.length();
            generator.emitExpressionInfo(newDivot, divotStart(), newDivot);
            generator.move(base.get(), generator.emitResolveScope(base.get(), var));
            generator.emitGetFromScope(tag.get(), base.get(), var, ThrowIfNotFound);
            generator.emitTDZCheckIfNecessary(var, tag.get(), nullptr);
        }
    } else if (m_tag->isBracketAccessorNode()) {
        BracketAccessorNode* bracket = static_cast<BracketAccessorNode*>(m_tag);
        base = generator.newTemporary();
        base = generator.emitNode(base.get(), bracket->base());
        RefPtr<RegisterID> property = generator.emitNodeForProperty(bracket->subscript());
        if (bracket->base()->isSuperNode()) {
            RefPtr<RegisterID> thisValue = generator.ensureThis();
            tag = generator.emitGetByVal(generator.newTemporary(), base.get(), thisValue.get(), property.get());
        } else
            tag = generator.emitGetByVal(generator.newTemporary(), base.get(), property.get());
    } else {
        ASSERT(m_tag->isDotAccessorNode());
        DotAccessorNode* dot = static_cast<DotAccessorNode*>(m_tag);
        tag = generator.newTemporary();
        base = generator.newTemporary();
        base = generator.emitNode(base.get(), dot->base());
        tag = dot->emitGetPropertyValue(generator, tag.get(), base.get());
    }

    RefPtr<RegisterID> templateObject = generator.emitGetTemplateObject(nullptr, this);

    unsigned expressionsCount = 0;
    for (TemplateExpressionListNode* templateExpression = m_templateLiteral->templateExpressions(); templateExpression; templateExpression = templateExpression->next())
        ++expressionsCount;

    CallArguments callArguments(generator, nullptr, 1 + expressionsCount);
    if (base)
        generator.move(callArguments.thisRegister(), base.get());
    else
        generator.emitLoad(callArguments.thisRegister(), jsUndefined());

    unsigned argumentIndex = 0;
    generator.move(callArguments.argumentRegister(argumentIndex++), templateObject.get());
    for (TemplateExpressionListNode* templateExpression = m_templateLiteral->templateExpressions(); templateExpression; templateExpression = templateExpression->next())
        generator.emitNode(callArguments.argumentRegister(argumentIndex++), templateExpression->value());

    return generator.emitCallInTailPosition(generator.finalDestination(dst, tag.get()), tag.get(), expectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
}

// ------------------------------ ArrayNode ------------------------------------

RegisterID* ArrayNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    bool hadVariableExpression = false;
    bool allDenseStrings = true;
    unsigned length = 0;

    IndexingType recommendedIndexingType = ArrayWithUndecided;
    ElementNode* firstPutElement;
    for (firstPutElement = m_element; firstPutElement; firstPutElement = firstPutElement->next()) {
        if (firstPutElement->elision() || firstPutElement->value()->isSpreadExpression())
            break;
        if (!firstPutElement->value()->isConstant())
            hadVariableExpression = true;
        else {
            JSValue constant = static_cast<ConstantNode*>(firstPutElement->value())->jsValue(generator);
            if (!constant) [[unlikely]]
                hadVariableExpression = true;
            else {
                recommendedIndexingType = leastUpperBoundOfIndexingTypeAndValue(recommendedIndexingType, constant);
                if (!constant.isString())
                    allDenseStrings = false;
                else if (auto* impl = asString(constant)->tryGetValueImpl(); !impl || !impl->isAtom())
                    allDenseStrings = false;
            }
        }

        ++length;
    }
    if (hadVariableExpression)
        allDenseStrings = false;

    auto newArray = [&](RegisterID* dst, ElementNode* elements, unsigned length) {
        if (length && !hadVariableExpression) {
            VM& vm = generator.vm();
            recommendedIndexingType |= CopyOnWrite;
            ASSERT(vm.heap.isDeferred()); // We run bytecode generator under a DeferGC. If we stopped doing that, we'd need to put a DeferGC here as we filled in these slots.

            Structure* immutableButterflyStructure = allDenseStrings ? vm.immutableButterflyOnlyAtomStringsStructure.get() : vm.immutableButterflyStructure(recommendedIndexingType);
            auto* array = JSImmutableButterfly::tryCreate(generator.vm(), immutableButterflyStructure, length);
            RELEASE_ASSERT(array);

            unsigned index = 0;
            for (ElementNode* element = elements; index < length; element = element->next()) {
                ASSERT(element->value()->isConstant());
                JSValue constant = static_cast<ConstantNode*>(element->value())->jsValue(generator);
                ASSERT(constant);
                if (allDenseStrings) {
                    JSString* string = asString(constant);
                    StringImpl* stringImpl = const_cast<StringImpl*>(string->getValueImpl());
                    constant = vm.atomStringToJSStringMap.ensureValue(stringImpl, [&] { return string; });
                }
                array->setIndex(generator.vm(), index++, constant);
            }
            return generator.emitNewArrayBuffer(dst, array, recommendedIndexingType);
        }
        return generator.emitNewArray(dst, elements, length, recommendedIndexingType);
    };

    if (!firstPutElement && !m_elision)
        return newArray(generator.finalDestination(dst), m_element, length);

    allDenseStrings = false;

    if (firstPutElement && firstPutElement->value()->isSpreadExpression()) {
        bool hasElision = m_elision;
        if (!hasElision) {
            for (ElementNode* node = firstPutElement; node; node = node->next()) {
                if (node->elision()) {
                    hasElision = true;
                    break;
                }
            }
        }

        if (!hasElision)
            return generator.emitNewArrayWithSpread(generator.finalDestination(dst), m_element);
    }

    RefPtr<RegisterID> array = newArray(generator.tempDestination(dst), m_element, length);
    ElementNode* n = firstPutElement;
    for (; n; n = n->next()) {
        if (n->value()->isSpreadExpression())
            goto handleSpread;
        RefPtr<RegisterID> value = generator.emitNode(n->value());
        length += n->elision();

        RefPtr<RegisterID> index = generator.emitLoad(nullptr, jsNumber(length++));
        generator.emitDirectPutByVal(array.get(), index.get(), value.get());
    }

    if (m_elision) {
        RegisterID* value = generator.emitLoad(nullptr, jsNumber(m_elision + length));
        generator.emitPutById(array.get(), generator.propertyNames().length, value);
    }

    return generator.move(dst, array.get());
    
handleSpread:
    RefPtr<RegisterID> index = generator.emitLoad(generator.newTemporary(), jsNumber(length));
    auto spreader = scopedLambda<void(BytecodeGenerator&, RegisterID*)>([array, index](BytecodeGenerator& generator, RegisterID* value)
    {
        generator.emitDirectPutByVal(array.get(), index.get(), value);
        generator.emitInc(index.get());
    });
    for (; n; n = n->next()) {
        if (n->elision())
            generator.emitBinaryOp<OpAdd>(index.get(), index.get(), generator.emitLoad(nullptr, jsNumber(n->elision())), OperandTypes(ResultType::numberTypeIsInt32(), ResultType::numberTypeIsInt32()));
        if (n->value()->isSpreadExpression()) {
            SpreadExpressionNode* spread = static_cast<SpreadExpressionNode*>(n->value());
            generator.emitEnumeration(spread, spread->expression(), spreader);
        } else {
            generator.emitDirectPutByVal(array.get(), index.get(), generator.emitNode(n->value()));
            generator.emitInc(index.get());
        }
    }
    
    if (m_elision) {
        generator.emitBinaryOp<OpAdd>(index.get(), index.get(), generator.emitLoad(nullptr, jsNumber(m_elision)), OperandTypes(ResultType::numberTypeIsInt32(), ResultType::numberTypeIsInt32()));
        generator.emitPutById(array.get(), generator.propertyNames().length, index.get());
    }
    return generator.move(dst, array.get());
}

bool ArrayNode::isSimpleArray() const
{
    if (m_elision)
        return false;
    for (ElementNode* ptr = m_element; ptr; ptr = ptr->next()) {
        if (ptr->elision())
            return false;
        if (ptr->value()->isSpreadExpression())
            return false;
    }
    return true;
}

ArgumentListNode* ArrayNode::toArgumentList(ParserArena& parserArena, int lineNumber, int startPosition) const
{
    ASSERT(!m_elision);
    ElementNode* ptr = m_element;
    if (!ptr)
        return nullptr;
    JSTokenLocation location;
    location.line = lineNumber;
    location.startOffset = startPosition;
    ArgumentListNode* head = new (parserArena) ArgumentListNode(location, ptr->value());
    ArgumentListNode* tail = head;
    ptr = ptr->next();
    for (; ptr; ptr = ptr->next()) {
        ASSERT(!ptr->elision());
        tail = new (parserArena) ArgumentListNode(location, tail, ptr->value());
    }
    return head;
}

// ------------------------------ ObjectLiteralNode ----------------------------

RegisterID* ObjectLiteralNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (!m_list) {
        if (dst == generator.ignoredResult())
            return nullptr;
        return generator.emitNewObject(generator.finalDestination(dst));
    }

    auto* propertyList = m_list;
    RefPtr<RegisterID> newObject;
    if (propertyList->m_node->m_type & PropertyNode::Spread) {
        // Only one element and it is spread.
        if (!propertyList->m_next) {
            RefPtr<RegisterID> function = generator.moveLinkTimeConstant(nullptr, LinkTimeConstant::cloneObject);
            RefPtr<RegisterID> src = generator.emitNode(static_cast<ObjectSpreadExpressionNode*>(propertyList->m_node->m_assign)->expression());
            CallArguments args(generator, nullptr, 0);
            generator.move(args.thisRegister(), src.get());
            return generator.emitCall(generator.finalDestination(dst, function.get()), function.get(), NoExpectedFunction, args, position(), position(), position(), DebuggableCall::No);
        }

        bool foundNonConstant = false;
        for (auto* p = propertyList->m_next; p; p = p->m_next) {
            if (p->m_node->m_type & PropertyNode::Constant)
                continue;
            if (p->m_node->m_type & PropertyNode::Computed)
                continue;
            if (p->m_node->m_type & PropertyNode::Spread)
                continue;
            foundNonConstant = true;
            break;
        }

        // All properties are simple constants, and the first property is spread.
        // Let's first clone an object and materialize the rest.
        if (!foundNonConstant) {
            RefPtr<RegisterID> function = generator.moveLinkTimeConstant(nullptr, LinkTimeConstant::cloneObject);
            RefPtr<RegisterID> src = generator.emitNode(static_cast<ObjectSpreadExpressionNode*>(propertyList->m_node->m_assign)->expression());
            CallArguments args(generator, nullptr, 0);
            generator.move(args.thisRegister(), src.get());
            newObject = generator.emitCall(generator.tempDestination(dst), function.get(), NoExpectedFunction, args, position(), position(), position(), DebuggableCall::No);
            propertyList = propertyList->m_next;
        }
    }

    if (!newObject)
        newObject = generator.emitNewObject(generator.tempDestination(dst));
    generator.emitNode(newObject.get(), propertyList);
    return generator.move(dst, newObject.get());
}

// ------------------------------ PropertyListNode -----------------------------

static inline void emitPutHomeObject(BytecodeGenerator& generator, RegisterID* function, RegisterID* homeObject)
{
    generator.emitPutById(function, generator.propertyNames().builtinNames().homeObjectPrivateName(), homeObject);
}

void PropertyListNode::emitDeclarePrivateFieldNames(BytecodeGenerator& generator, RegisterID* scope)
{
    // Walk the list and declare any Private property names (e.g. `#foo`) in the provided scope.
    RefPtr<RegisterID> createPrivateSymbol;
    for (PropertyListNode* p = this; p; p = p->m_next) {
        const PropertyNode& node = *p->m_node;
        if (node.type() & PropertyNode::PrivateField) {
            if (!createPrivateSymbol)
                createPrivateSymbol = generator.moveLinkTimeConstant(nullptr, LinkTimeConstant::createPrivateSymbol);

            CallArguments arguments(generator, nullptr, 1);
            generator.emitLoad(arguments.thisRegister(), jsUndefined());
            generator.emitLoad(arguments.argumentRegister(0), *node.name());
            RefPtr<RegisterID> symbol = generator.emitCall(generator.finalDestination(nullptr, createPrivateSymbol.get()), createPrivateSymbol.get(), NoExpectedFunction, arguments, position(), position(), position(), DebuggableCall::No);

            Variable var = generator.variable(*node.name());
            generator.emitPutToScope(scope, var, symbol.get(), DoNotThrowIfNotFound, InitializationMode::ConstInitialization);
        }
    }
}

static ALWAYS_INLINE bool needsHomeObject(ExpressionNode* node)
{
    if (node->isBaseFuncExprNode())
        return static_cast<BaseFuncExprNode*>(node)->metadata()->superBinding() == SuperBinding::Needed;
    return false;
}

RegisterID* PropertyListNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dstOrConstructor, RegisterID* prototype, Vector<UnlinkedFunctionExecutable::ClassElementDefinition>* instanceElementDefinitions, Vector<UnlinkedFunctionExecutable::ClassElementDefinition>* staticElementDefinitions)
{
    auto makeClassElementDefinition = [](const PropertyListNode* p) {
        using Kind = UnlinkedFunctionExecutable::ClassElementDefinition::Kind;

        std::optional<JSTextPosition> initializerPosition = std::nullopt;
        if (ExpressionNode* initializer = p->m_node->m_assign)
            initializerPosition = initializer->position();

        Kind kind = Kind::FieldWithLiteralPropertyKey;
        if (p->m_node->isStaticClassBlock())
            kind = Kind::StaticInitializationBlock;
        else if (p->m_node->hasComputedName())
            kind = Kind::FieldWithComputedPropertyKey;
        else if (p->m_node->isPrivate())
            kind = Kind::FieldWithPrivatePropertyKey;

        return UnlinkedFunctionExecutable::ClassElementDefinition { *p->m_node->name(), p->position(), initializerPosition, kind };
    };

    using GetterSetterPair = std::pair<PropertyNode*, PropertyNode*>;
    using GetterSetterMap = UncheckedKeyHashMap<UniquedStringImpl*, GetterSetterPair, IdentifierRepHash>;

    if (hasPrivateAccessors()) {
        GetterSetterMap privateAccessorMap;

        for (PropertyListNode* propertyList = this; propertyList; propertyList = propertyList->m_next) {
            if (!(propertyList->m_node->type() & (PropertyNode::PrivateGetter | PropertyNode::PrivateSetter)))
                continue;

            // We group private getters and setters to store them in a object
            GetterSetterPair pair(propertyList->m_node, static_cast<PropertyNode*>(nullptr));
            GetterSetterMap::AddResult result = privateAccessorMap.add(propertyList->m_node->name()->impl(), pair);
            auto& resultPair = result.iterator->value;
            // If the map already contains an element with node->name(),
            // we need to store this node in the second part.
            if (!result.isNewEntry)
                resultPair.second = propertyList->m_node;
            continue;
        }

        // Then we declare private accessors
        for (auto& it : privateAccessorMap) {
            // FIXME: Use GetterSetter to store private accessors
            // https://bugs.webkit.org/show_bug.cgi?id=221915
            RefPtr<RegisterID> getterSetterObj = generator.emitNewObject(generator.newTemporary());
            GetterSetterPair pair = it.value;

            auto emitPutAccessor = [&] (PropertyNode* propertyNode) {
                RegisterID* base = propertyNode->isInstanceClassProperty() ? prototype : dstOrConstructor;

                RefPtr<RegisterID> value = generator.emitNode(propertyNode->m_assign);
                if (propertyNode->needsSuperBinding())
                    emitPutHomeObject(generator, value.get(), base);
                auto setterOrGetterIdent = propertyNode->m_type & PropertyNode::PrivateGetter
                    ? generator.propertyNames().builtinNames().getPrivateName()
                    : generator.propertyNames().builtinNames().setPrivateName();
                generator.emitDirectPutById(getterSetterObj.get(), setterOrGetterIdent, value.get());
            };

            if (pair.first)
                emitPutAccessor(pair.first);

            if (pair.second)
                emitPutAccessor(pair.second);

            Variable var = generator.variable(*pair.first->name());
            generator.emitPutToScope(generator.scopeRegister(), var, getterSetterObj.get(), DoNotThrowIfNotFound, InitializationMode::ConstInitialization);
        }
    }

    PropertyListNode* p = this;
    RegisterID* dst = nullptr;

    // Fast case: this loop just handles regular value properties.
    for (; p && (p->m_node->m_type & PropertyNode::Constant); p = p->m_next) {
        dst = p->m_node->isInstanceClassProperty() ? prototype : dstOrConstructor;

        if (p->m_node->type() & (PropertyNode::PrivateGetter | PropertyNode::PrivateSetter))
            continue;

        if (p->isComputedClassField())
            emitSaveComputedFieldName(generator, *p->m_node);

        if (p->isInstanceClassField() && !(p->m_node->type() & PropertyNode::PrivateMethod)) {
            ASSERT(instanceElementDefinitions);
            instanceElementDefinitions->append(makeClassElementDefinition(p));
            continue;
        }

        if (p->isStaticClassElement()) {
            ASSERT(staticElementDefinitions);
            staticElementDefinitions->append(makeClassElementDefinition(p));
            continue;
        }

        emitPutConstantProperty(generator, dst, *p->m_node);
    }

    // Were there any get/set properties?
    if (p) {
        // Build a list of getter/setter pairs to try to put them at the same time. If we encounter
        // a constant property by the same name as accessor or a computed property or a spread,
        // just emit everything as that may override previous values.
        bool canOverrideProperties = false;

        GetterSetterMap instanceMap;
        GetterSetterMap staticMap;

        // Build a map, pairing get/set values together.
        for (PropertyListNode* q = p; q; q = q->m_next) {
            PropertyNode* node = q->m_node;
            if (node->m_type & PropertyNode::Computed || node->m_type & PropertyNode::Spread) {
                canOverrideProperties = true;
                break;
            }

            GetterSetterMap& map = node->isStaticClassProperty() ? staticMap : instanceMap;
            if (node->m_type & PropertyNode::Constant) {
                if (map.contains(node->name()->impl())) {
                    canOverrideProperties = true;
                    break;
                }
                continue;
            }

            // Duplicates are possible.
            GetterSetterPair pair(node, static_cast<PropertyNode*>(nullptr));
            GetterSetterMap::AddResult result = map.add(node->name()->impl(), pair);
            auto& resultPair = result.iterator->value;
            if (!result.isNewEntry) {
                if (resultPair.first->m_type == node->m_type) {
                    resultPair.first->setIsOverriddenByDuplicate();
                    resultPair.first = node;
                } else {
                    if (resultPair.second)
                        resultPair.second->setIsOverriddenByDuplicate();
                    resultPair.second = node;
                }
            }
        }

        // Iterate over the remaining properties in the list.
        for (; p; p = p->m_next) {
            PropertyNode* node = p->m_node;
            dst = node->isInstanceClassProperty() ? prototype : dstOrConstructor;

            if (p->isComputedClassField())
                emitSaveComputedFieldName(generator, *p->m_node);

            if (p->m_node->type() & (PropertyNode::PrivateGetter | PropertyNode::PrivateSetter))
                continue;

            if (p->isInstanceClassField()) {
                ASSERT(instanceElementDefinitions);
                ASSERT(node->m_type & PropertyNode::Constant);
                instanceElementDefinitions->append(makeClassElementDefinition(p));
                continue;
            }

            if (p->isStaticClassElement()) {
                ASSERT(staticElementDefinitions);
                staticElementDefinitions->append(makeClassElementDefinition(p));
                continue;
            }

            // Handle regular values.
            if (node->m_type & PropertyNode::Constant) {
                emitPutConstantProperty(generator, dst, *node);
                continue;
            } else if (node->m_type & PropertyNode::Spread) {
                generator.emitNode(dst, node->m_assign);
                continue;
            }

            RefPtr<RegisterID> value = generator.emitNode(node->m_assign);
            if (needsHomeObject(node->m_assign))
                emitPutHomeObject(generator, value.get(), dst);

            unsigned attributes = node->isClassProperty() ? (PropertyAttribute::Accessor | PropertyAttribute::DontEnum) : static_cast<unsigned>(PropertyAttribute::Accessor);

            ASSERT(node->m_type & (PropertyNode::Getter | PropertyNode::Setter));

            // This is a get/set property which may be overridden by a computed property or spread later.
            if (canOverrideProperties) {
                // Computed accessors.
                if (node->m_type & PropertyNode::Computed) {
                    RefPtr<RegisterID> propertyName = generator.emitNode(node->m_expression);
                    if (generator.shouldSetFunctionName(node->m_assign)) {
                        propertyName = generator.emitToPropertyKey(generator.newTemporary(), propertyName.get());
                        generator.emitSetFunctionName(value.get(), propertyName.get());
                    }
                    if (node->m_type & PropertyNode::Getter)
                        generator.emitPutGetterByVal(dst, propertyName.get(), attributes, value.get());
                    else
                        generator.emitPutSetterByVal(dst, propertyName.get(), attributes, value.get());
                    continue;
                }

                if (node->m_type & PropertyNode::Getter)
                    generator.emitPutGetterById(dst, *node->name(), attributes, value.get());
                else
                    generator.emitPutSetterById(dst, *node->name(), attributes, value.get());
                continue;
            }

            // This is a get/set property pair.
            GetterSetterMap& map = node->isStaticClassProperty() ? staticMap : instanceMap;
            GetterSetterMap::iterator it = map.find(node->name()->impl());
            ASSERT(it != map.end());
            GetterSetterPair& pair = it->value;

            // Was this already generated as a part of its partner?
            if (pair.second == node || node->isOverriddenByDuplicate())
                continue;

            // Generate the paired node now.
            RefPtr<RegisterID> getterReg;
            RefPtr<RegisterID> setterReg;
            RegisterID* secondReg = nullptr;

            if (node->m_type & PropertyNode::Getter) {
                getterReg = value;
                if (pair.second) {
                    ASSERT(pair.second->m_type & PropertyNode::Setter);
                    setterReg = generator.emitNode(pair.second->m_assign);
                    secondReg = setterReg.get();
                } else
                    setterReg = generator.emitLoad(nullptr, jsUndefined());
            } else {
                ASSERT(node->m_type & PropertyNode::Setter);
                setterReg = value;
                if (pair.second) {
                    ASSERT(pair.second->m_type & PropertyNode::Getter);
                    getterReg = generator.emitNode(pair.second->m_assign);
                    secondReg = getterReg.get();
                } else
                    getterReg = generator.emitLoad(nullptr, jsUndefined());
            }

            if (pair.second) {
                if (needsHomeObject(pair.second->m_assign))
                    emitPutHomeObject(generator, secondReg, dst);
            }

            generator.emitPutGetterSetter(dst, *node->name(), attributes, getterReg.get(), setterReg.get());
        }
    }

    return dstOrConstructor;
}

void PropertyListNode::emitPutConstantProperty(BytecodeGenerator& generator, RegisterID* newObj, PropertyNode& node)
{
    // Private fields are handled in a synthetic classFieldInitializer function, not here.
    ASSERT(!(node.type() & PropertyNode::PrivateField));

    if (PropertyNode::isUnderscoreProtoSetter(generator.vm(), node)) {
        RefPtr<RegisterID> prototype = generator.emitNode(node.m_assign);
        generator.emitDirectSetPrototypeOf<InvalidPrototypeMode::Ignore>(newObj, prototype.get(), m_position, m_position, m_position);
        return;
    }

    bool shouldSetFunctionName = generator.shouldSetFunctionName(node.m_assign);

    RefPtr<RegisterID> propertyName;
    if (!node.name()) {
        propertyName = generator.newTemporary();
        if (shouldSetFunctionName)
            generator.emitToPropertyKey(propertyName.get(), generator.emitNode(node.m_expression));
        else
            generator.emitNode(propertyName.get(), node.m_expression);
    }

    RefPtr<RegisterID> value = generator.emitNode(node.m_assign);
    if (needsHomeObject(node.m_assign))
        emitPutHomeObject(generator, value.get(), newObj);

    if (node.isClassProperty()) {
        ASSERT(node.needsSuperBinding());
        ASSERT(!(node.type() & PropertyNode::PrivateSetter));
        ASSERT(!(node.type() & PropertyNode::PrivateGetter));

        if (node.type() & PropertyNode::PrivateMethod) {
            Variable var = generator.variable(*node.name());
            generator.emitPutToScope(generator.scopeRegister(), var, value.get(), DoNotThrowIfNotFound, InitializationMode::ConstInitialization);
            return;
        }

        RefPtr<RegisterID> propertyNameRegister;
        if (node.name())
            propertyName = generator.emitLoad(nullptr, *node.name());

        if (shouldSetFunctionName)
            generator.emitSetFunctionName(value.get(), propertyName.get());
        generator.emitCallDefineProperty(newObj, propertyName.get(), value.get(), nullptr, nullptr, BytecodeGenerator::PropertyConfigurable | BytecodeGenerator::PropertyWritable, m_position);
        return;
    }

    if (const auto* identifier = node.name()) {
        ASSERT(!propertyName);
        std::optional<uint32_t> optionalIndex = parseIndex(*identifier);
        if (!optionalIndex) {
            generator.emitDirectPutById(newObj, *identifier, value.get());
            return;
        }

        propertyName = generator.emitLoad(nullptr, jsNumber(optionalIndex.value()));
        generator.emitDirectPutByVal(newObj, propertyName.get(), value.get());
        return;
    }

    if (shouldSetFunctionName)
        generator.emitSetFunctionName(value.get(), propertyName.get());
    generator.emitDirectPutByVal(newObj, propertyName.get(), value.get());
}

void PropertyListNode::emitSaveComputedFieldName(BytecodeGenerator& generator, PropertyNode& node)
{
    ASSERT(node.isComputedClassField());

    // The 'name' refers to a synthetic private name in the class scope, where the property key is saved for later use.
    const Identifier& description = *node.name();
    Variable var = generator.variable(description);
    ASSERT(!var.local());

    RefPtr<RegisterID> propertyExpr = generator.emitNode(node.m_expression);
    RefPtr<RegisterID> propertyName = generator.emitToPropertyKey(generator.newTemporary(), propertyExpr.get());

    if (node.isStaticClassField()) {
        Ref<Label> validPropertyNameLabel = generator.newLabel();
        RefPtr<RegisterID> prototypeString = generator.emitLoad(nullptr, JSValue(generator.addStringConstant(generator.propertyNames().prototype)));
        generator.emitJumpIfFalse(generator.emitBinaryOp<OpStricteq>(generator.newTemporary(), prototypeString.get(), propertyName.get(), OperandTypes(ResultType::stringType(), ResultType::stringType())), validPropertyNameLabel.get());
        generator.emitThrowTypeError("Cannot declare a static field named 'prototype'"_s);
        generator.emitLabel(validPropertyNameLabel.get());
    }

    RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
    generator.emitPutToScope(scope.get(), var, propertyName.get(), ThrowIfNotFound, InitializationMode::ConstInitialization);
}

// ------------------------------ BracketAccessorNode --------------------------------

static bool isNonIndexStringElement(ExpressionNode& element)
{
    return element.isString() && !parseIndex(static_cast<StringNode&>(element).value());
}

RegisterID* BracketAccessorNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (m_base->isSuperNode()) {
        RefPtr<RegisterID> finalDest = generator.finalDestination(dst);
        RefPtr<RegisterID> thisValue = generator.ensureThis();
        RefPtr<RegisterID> superBase = emitSuperBaseForCallee(generator);

        if (isNonIndexStringElement(*m_subscript)) {
            const Identifier& id = static_cast<StringNode*>(m_subscript)->value();
            generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
            generator.emitGetById(finalDest.get(), superBase.get(), thisValue.get(), id);
        } else  {
            RefPtr<RegisterID> subscript = generator.emitNodeForProperty(m_subscript);
            generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
            generator.emitGetByVal(finalDest.get(), superBase.get(), thisValue.get(), subscript.get());
        }

        generator.emitProfileType(finalDest.get(), divotStart(), divotEnd());
        return finalDest.get();
    }

    RegisterID* ret;
    RefPtr<RegisterID> finalDest = generator.finalDestination(dst);

    bool subscriptIsNonIndexString = isNonIndexStringElement(*m_subscript);
    RefPtr<RegisterID> base = subscriptIsNonIndexString
        ? generator.emitNode(m_base)
        : generator.emitNodeForLeftHandSide(m_base, m_subscriptHasAssignments, m_subscript->isPure(generator));

    if (m_base->isOptionalChainBase())
        generator.emitOptionalCheck(base.get());

    if (subscriptIsNonIndexString) {
        generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
        ret = generator.emitGetById(finalDest.get(), base.get(), static_cast<StringNode*>(m_subscript)->value());
    } else {
        RegisterID* property = generator.emitNodeForProperty(m_subscript);
        generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
        ret = generator.emitGetByVal(finalDest.get(), base.get(), property);
    }

    generator.emitProfileType(finalDest.get(), divotStart(), divotEnd());
    return ret;
}

// ------------------------------ DotAccessorNode --------------------------------

RegisterID* DotAccessorNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> finalDest = generator.finalDestination(dst);

    if (generator.shouldGetArgumentsDotLengthFast(this))
        return generator.emitArgumentCount(finalDest.get());

    bool baseIsSuper = m_base->isSuperNode();

    RefPtr<RegisterID> base;
    if (baseIsSuper)
        base = emitSuperBaseForCallee(generator);
    else {
        base = generator.emitNode(m_base);
        if (m_base->isOptionalChainBase())
            generator.emitOptionalCheck(base.get());
    }

    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RegisterID* ret = emitGetPropertyValue(generator, finalDest.get(), base.get());

    generator.emitProfileType(finalDest.get(), divotStart(), divotEnd());
    return ret;
}

RegisterID* BaseDotNode::emitGetPropertyValue(BytecodeGenerator& generator, RegisterID* dst, RegisterID* base, RefPtr<RegisterID>& thisValue)
{
    if (isPrivateMember()) {
        auto identifierName = identifier();
        auto privateTraits = generator.getPrivateTraits(identifierName);
        if (privateTraits.isMethod()) {
            Variable var = generator.variable(identifierName);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            ASSERT(scope); // Private names are always captured.
            RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
            generator.emitCheckPrivateBrand(base, privateBrandSymbol.get(), privateTraits.isStatic());

            return generator.emitGetFromScope(dst, scope.get(), var, ThrowIfNotFound);
        }

        if (privateTraits.isGetter()) {
            Variable var = generator.variable(identifierName);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            ASSERT(scope); // Private names are always captured.
            RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
            generator.emitCheckPrivateBrand(base, privateBrandSymbol.get(), privateTraits.isStatic());

            RefPtr<RegisterID> getterSetterObj = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
            RefPtr<RegisterID> getterFunction = generator.emitDirectGetById(generator.newTemporary(), getterSetterObj.get(), generator.propertyNames().builtinNames().getPrivateName());
            CallArguments args(generator, nullptr);
            generator.move(args.thisRegister(), base);
            return generator.emitCall(dst, getterFunction.get(), NoExpectedFunction, args, m_position, m_position, m_position, DebuggableCall::Yes);
        }

        if (privateTraits.isSetter()) {
            // We need to perform brand check to follow the spec
            Variable var = generator.variable(identifierName);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            ASSERT(scope); // Private names are always captured.
            RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
            generator.emitCheckPrivateBrand(base, privateBrandSymbol.get(), privateTraits.isStatic());
            generator.emitThrowTypeError("Trying to access an undefined private getter"_s);
            return dst;
        }

        ASSERT(privateTraits.isField());
        Variable var = generator.variable(m_ident);
        ASSERT_WITH_MESSAGE(!var.local(), "Private Field names must be stored in captured variables");

        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        ASSERT(scope); // Private names are always captured.
        RefPtr<RegisterID> privateName = generator.newTemporary();
        generator.emitGetFromScope(privateName.get(), scope.get(), var, DoNotThrowIfNotFound);
        return generator.emitGetPrivateName(dst, base, privateName.get());
    }

    if (m_base->isSuperNode()) {
        if (!thisValue)
            thisValue = generator.ensureThis();
        return generator.emitGetById(dst, base, thisValue.get(), m_ident);
    }

    return generator.emitGetById(dst, base, m_ident);
}

RegisterID* BaseDotNode::emitGetPropertyValue(BytecodeGenerator& generator, RegisterID* dst, RegisterID* base)
{
    RefPtr<RegisterID> thisValue;
    return emitGetPropertyValue(generator, dst, base, thisValue);
}

RegisterID* BaseDotNode::emitPutProperty(BytecodeGenerator& generator, RegisterID* base, RegisterID* value, RefPtr<RegisterID>& thisValue)
{
    if (isPrivateMember()) {
        auto identifierName = identifier();
        auto privateTraits = generator.getPrivateTraits(identifierName);
        if (privateTraits.isSetter()) {
            Variable var = generator.variable(identifierName);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            ASSERT(scope); // Private names are always captured.
            RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
            generator.emitCheckPrivateBrand(base, privateBrandSymbol.get(), privateTraits.isStatic());

            RefPtr<RegisterID> getterSetterObj = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
            RefPtr<RegisterID> setterFunction = generator.emitDirectGetById(generator.newTemporary(), getterSetterObj.get(), generator.propertyNames().builtinNames().setPrivateName());
            CallArguments args(generator, nullptr, 1);
            generator.move(args.thisRegister(), base);
            generator.move(args.argumentRegister(0), value);
            generator.emitCallIgnoreResult(generator.newTemporary(), setterFunction.get(), NoExpectedFunction, args, m_position, m_position, m_position, DebuggableCall::Yes);

            return value;
        }

        if (privateTraits.isGetter() || privateTraits.isMethod()) {
            Variable var = generator.variable(identifierName);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            ASSERT(scope); // Private names are always captured.
            RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
            generator.emitCheckPrivateBrand(base, privateBrandSymbol.get(), privateTraits.isStatic());

            generator.emitThrowTypeError("Trying to access an undefined private setter"_s);
            return value;
        }

        ASSERT(privateTraits.isField());
        Variable var = generator.variable(m_ident);
        ASSERT_WITH_MESSAGE(!var.local(), "Private Field names must be stored in captured variables");

        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        ASSERT(scope); // Private names are always captured.
        RefPtr<RegisterID> privateName = generator.newTemporary();
        generator.emitGetFromScope(privateName.get(), scope.get(), var, DoNotThrowIfNotFound);
        return generator.emitPrivateFieldPut(base, privateName.get(), value);
    }

    if (m_base->isSuperNode()) {
        if (!thisValue)
            thisValue = generator.ensureThis();
        return generator.emitPutById(base, thisValue.get(), m_ident, value);
    }

    return generator.emitPutById(base, m_ident, value);
}

RegisterID* BaseDotNode::emitPutProperty(BytecodeGenerator& generator, RegisterID* base, RegisterID* value)
{
    RefPtr<RegisterID> thisValue;
    return emitPutProperty(generator, base, value, thisValue);
}

// ------------------------------ ArgumentListNode -----------------------------

RegisterID* ArgumentListNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(m_expr);
    return generator.emitNode(dst, m_expr);
}

// ------------------------------ NewExprNode ----------------------------------

RegisterID* NewExprNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ExpectedFunction expectedFunction;
    if (m_expr->isResolveNode())
        expectedFunction = generator.expectedFunctionForIdentifier(static_cast<ResolveNode*>(m_expr)->identifier());
    else
        expectedFunction = NoExpectedFunction;

    RefPtr<RegisterID> func = nullptr;
    if (m_args && m_args->hasAssignments())
        func = generator.newTemporary();
    func = generator.emitNode(func.get(), m_expr);
    RefPtr<RegisterID> returnValue = generator.finalDestination(dst, func.get());
    CallArguments callArguments(generator, m_args);
    return generator.emitConstruct(returnValue.get(), func.get(), func.get(), expectedFunction, callArguments, divot(), divotStart(), divotEnd());
}

CallArguments::CallArguments(BytecodeGenerator& generator, ArgumentsNode* argumentsNode, unsigned additionalArguments)
    : m_argumentsNode(argumentsNode)
{
    size_t argumentCountIncludingThis = 1 + additionalArguments; // 'this' register.
    if (argumentsNode) {
        for (ArgumentListNode* node = argumentsNode->m_listNode; node; node = node->m_next)
            ++argumentCountIncludingThis;
    }

    static_assert(stackAlignmentRegisters() == 2);
    size_t argvSize = argumentCountIncludingThis;
    ASSERT(argvSize >= 1);
    if ((CallFrame::headerSizeInRegisters + argvSize) % stackAlignmentRegisters())
        ++argvSize;
    argvSize += 1; // For stackOffset adjustment case.
    ASSERT(argvSize >= 2);
    m_allocatedRegisters.grow(argvSize);

    // Do not initialize index 0.
    size_t index = m_allocatedRegisters.size();
    generator.newTemporaries(m_allocatedRegisters.size() - 1, [&](RegisterID* slot) {
        m_allocatedRegisters[--index] = slot;
    });

    // We initialize 0 based on offset. And adjust m_argv based on that.
    if ((-m_allocatedRegisters[1]->index() + CallFrame::headerSizeInRegisters) % stackAlignmentRegisters()) {
        m_allocatedRegisters[0] = generator.newTemporary();
        m_argv = m_allocatedRegisters.mutableSpan().first(argumentCountIncludingThis);
    } else
        m_argv = m_allocatedRegisters.mutableSpan().subspan(1, argumentCountIncludingThis);
}

// ------------------------------ EvalFunctionCallNode ----------------------------------

RegisterID* EvalFunctionCallNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    // We need try to load 'this' before call eval in constructor, because 'this' can created by 'super' in some of the arrow function
    // var A = class A {
    //   constructor () { this.id = 'A'; }
    // }
    //
    // var B = class B extend A {
    //    constructor () {
    //       var arrow = () => super();
    //       arrow();
    //       eval("this.id = 'B'");
    //    }
    // }
    if (generator.constructorKind() == ConstructorKind::Extends && generator.needsToUpdateArrowFunctionContext() && generator.isThisUsedInInnerArrowFunction())
        generator.emitLoadThisFromArrowFunctionLexicalEnvironment();

    Variable var = generator.variable(generator.propertyNames().eval);
    RefPtr<RegisterID> local = var.local();
    RefPtr<RegisterID> func;
    if (local) {
        generator.emitTDZCheckIfNecessary(var, local.get(), nullptr);
        func = generator.move(generator.tempDestination(dst), local.get());
    } else
        func = generator.newTemporary();
    CallArguments callArguments(generator, m_args);

    if (local)
        generator.emitLoad(callArguments.thisRegister(), jsUndefined());
    else {
        JSTextPosition newDivot = divotStart() + 4;
        generator.emitExpressionInfo(newDivot, divotStart(), newDivot);
        generator.move(
            callArguments.thisRegister(),
            generator.emitResolveScope(callArguments.thisRegister(), var));
        generator.emitGetFromScope(func.get(), callArguments.thisRegister(), var, ThrowIfNotFound);
        generator.emitTDZCheckIfNecessary(var, func.get(), nullptr);
    }

    RefPtr<RegisterID> returnValue = generator.finalDestination(dst, func.get());

    if (m_args->m_listNode && m_args->m_listNode->m_expr && m_args->m_listNode->m_expr->isSpreadExpression()) {
        Ref<Label> notEvalFunction = generator.newLabel();
        Ref<Label> done = generator.newLabel();
        generator.emitJumpIfNotEvalFunction(func.get(), notEvalFunction.get());

        {
            SpreadExpressionNode* spread = static_cast<SpreadExpressionNode*>(m_args->m_listNode->m_expr);
            RefPtr<RegisterID> spreadRegister = generator.emitNode(spread->expression());
            generator.emitExpressionInfo(spread->divot(), spread->divotStart(), spread->divotEnd());

            CallArguments directEvalArguments(generator, nullptr, 1);
            generator.move(directEvalArguments.thisRegister(), callArguments.thisRegister());
            generator.emitGetByVal(directEvalArguments.argumentRegister(0), spreadRegister.get(), generator.emitLoad(nullptr, jsNumber(0)));
            generator.emitCallDirectEval(returnValue.get(), func.get(), directEvalArguments, divot(), divotStart(), divotEnd(), DebuggableCall::No);
            generator.emitJump(done.get());
        }

        generator.emitLabel(notEvalFunction.get());
        generator.emitCallInTailPosition(returnValue.get(), func.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        generator.emitLabel(done.get());
    } else
        generator.emitCallDirectEval(returnValue.get(), func.get(), callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::No);

    return returnValue.get();
}

// ------------------------------ FunctionCallValueNode ----------------------------------

RegisterID* FunctionCallValueNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (m_expr->isSuperNode()) {
        RefPtr<RegisterID> func = emitGetSuperFunctionForConstruct(generator);
        RefPtr<RegisterID> returnValue = generator.finalDestination(dst, func.get());
        CallArguments callArguments(generator, m_args);

        ASSERT(generator.isConstructor() || generator.derivedContextType() == DerivedContextType::DerivedConstructorContext);
        ASSERT(generator.constructorKind() == ConstructorKind::Extends || generator.derivedContextType() == DerivedContextType::DerivedConstructorContext);
        RegisterID* ret = generator.emitSuperConstruct(returnValue.get(), func.get(), generator.newTarget(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd());

        bool isConstructorKindDerived = generator.constructorKind() == ConstructorKind::Extends;
        bool doWeUseArrowFunctionInConstructor = isConstructorKindDerived && generator.needsToUpdateArrowFunctionContext();

        if (generator.isDerivedConstructorContext() || (doWeUseArrowFunctionInConstructor && generator.isSuperCallUsedInInnerArrowFunction()))
            generator.emitLoadThisFromArrowFunctionLexicalEnvironment();
        
        Ref<Label> thisIsEmptyLabel = generator.newLabel();
        generator.emitJumpIfTrue(generator.emitIsEmpty(generator.newTemporary(), generator.thisRegister()), thisIsEmptyLabel.get());
        generator.emitThrowReferenceError("'super()' can't be called more than once in a constructor."_s);
        generator.emitLabel(thisIsEmptyLabel.get());

        generator.move(generator.thisRegister(), ret);
        
        if (generator.isDerivedConstructorContext() || doWeUseArrowFunctionInConstructor)
            generator.emitPutThisToArrowFunctionContextScope();

        // Initialize instance fields after super-call.
        if (generator.privateBrandRequirement() == PrivateBrandRequirement::Needed)
            generator.emitInstallPrivateBrand(generator.thisRegister());

        if (generator.needsClassFieldInitializer() == NeedsClassFieldInitializer::Yes) {
            ASSERT(generator.isConstructor() || generator.isDerivedConstructorContext());
            func = generator.emitLoadDerivedConstructor();
            generator.emitInstanceFieldInitializationIfNeeded(generator.thisRegister(), func.get(), divot(), divotStart(), divotEnd());
        }
        return ret;
    }

    RefPtr<RegisterID> func = nullptr;
    if (m_args && m_args->hasAssignments())
        func = generator.newTemporary();
    func = generator.emitNode(func.get(), m_expr);
    RefPtr<RegisterID> returnValue = generator.finalDestination(dst, func.get());
    if (isOptionalCall())
        generator.emitOptionalCheck(func.get());

    CallArguments callArguments(generator, m_args);
    generator.emitLoad(callArguments.thisRegister(), jsUndefined());
    RegisterID* ret = generator.emitCallInTailPosition(returnValue.get(), func.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
    generator.emitProfileType(returnValue.get(), divotStart(), divotEnd());
    return ret;
}

// ------------------------------ StaticBlockFunctionCallNode ----------------------------------

RegisterID* StaticBlockFunctionCallNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    // There are two possible optimizations in this implementation.
    // https://bugs.webkit.org/show_bug.cgi?id=245925
    RefPtr homeObject = emitHomeObjectForCallee(generator);
    RefPtr function = generator.emitNode(m_expr);
    emitPutHomeObject(generator, function.get(), homeObject.get()); 
    RefPtr returnValue = generator.finalDestination(dst, function.get());

    CallArguments callArguments(generator, nullptr);
    generator.move(callArguments.thisRegister(), generator.thisRegister());
    RefPtr result = generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);

    generator.emitProfileType(returnValue.get(), divotStart(), divotEnd());
    return result.get();
}

// ------------------------------ FunctionCallResolveNode ----------------------------------

RegisterID* FunctionCallResolveNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (!ASSERT_ENABLED) {
        if (m_ident == generator.vm().propertyNames->builtinNames().assertPrivateName()) [[unlikely]]
            return generator.move(dst, generator.emitLoad(nullptr, jsUndefined()));
    }

    ExpectedFunction expectedFunction = generator.expectedFunctionForIdentifier(m_ident);

    Variable var = generator.variable(m_ident);
    RefPtr<RegisterID> local = var.local();
    RefPtr<RegisterID> func;
    if (local) {
        generator.emitTDZCheckIfNecessary(var, local.get(), nullptr);
        if (m_args->hasAssignments())
            func = generator.move(generator.tempDestination(dst), local.get());
        else
            func = local;
    } else
        func = generator.tempDestination(dst);
    CallArguments callArguments(generator, m_args);

    if (local) {
        generator.emitLoad(callArguments.thisRegister(), jsUndefined());
        // This passes NoExpectedFunction because we expect that if the function is in a
        // local variable, then it's not one of our built-in constructors.
        expectedFunction = NoExpectedFunction;
    } else {
        JSTextPosition newDivot = divotStart() + m_ident.length();
        generator.emitExpressionInfo(newDivot, divotStart(), newDivot);
        generator.move(
            callArguments.thisRegister(),
            generator.emitResolveScope(callArguments.thisRegister(), var));
        generator.emitGetFromScope(func.get(), callArguments.thisRegister(), var, ThrowIfNotFound);
        generator.emitTDZCheckIfNecessary(var, func.get(), nullptr);
    }

    RefPtr<RegisterID> returnValue = generator.finalDestination(dst, func.get());
    if (isOptionalCall())
        generator.emitOptionalCheck(func.get());

    RegisterID* ret = generator.emitCallInTailPosition(returnValue.get(), func.get(), expectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
    generator.emitProfileType(returnValue.get(), divotStart(), divotEnd());
    return ret;
}

// ------------------------------ BytecodeIntrinsicNode ----------------------------------

RegisterID* BytecodeIntrinsicNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (m_entry.type() == BytecodeIntrinsicRegistry::Type::Emitter)
        return (this->*m_entry.emitter())(generator, dst);
    if (dst == generator.ignoredResult())
        return nullptr;
    return generator.moveLinkTimeConstant(dst, m_entry.linkTimeConstant());
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getByIdDirect(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    ASSERT(node->m_expr->isString());
    const Identifier& ident = static_cast<StringNode*>(node->m_expr)->value();
    ASSERT(!node->m_next);
    return generator.emitDirectGetById(generator.finalDestination(dst), base.get(), ident);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getByIdDirectPrivate(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    ASSERT(node->m_expr->isString());
    SymbolImpl* symbol = generator.vm().propertyNames->builtinNames().lookUpPrivateName(static_cast<StringNode*>(node->m_expr)->value());
    ASSERT(symbol);
    ASSERT(!node->m_next);
    return generator.emitDirectGetById(generator.finalDestination(dst), base.get(), generator.parserArena().identifierArena().makeIdentifier(generator.vm(), symbol));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getByValWithThis(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> thisValue = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> property = generator.emitNodeForProperty(node);

    ASSERT(!node->m_next);

    return generator.emitGetByVal(generator.finalDestination(dst), base.get(), thisValue.get(), property.get());
}

static ALWAYS_INLINE void emitIntrinsicPutByValWithThis(BytecodeGenerator& generator, ArgumentListNode* node, ECMAMode ecmaMode)
{
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> thisValue = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> property = generator.emitNodeForProperty(node);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNodeForProperty(node);

    ASSERT(!node->m_next);

    generator.emitPutByValWithECMAMode(base.get(), thisValue.get(), property.get(), value.get(), ecmaMode);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putByValWithThisSloppy(BytecodeGenerator& generator, RegisterID* dst)
{
    emitIntrinsicPutByValWithThis(generator, m_args->m_listNode, ECMAMode::sloppy());
    return dst;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putByValWithThisStrict(BytecodeGenerator& generator, RegisterID* dst)
{
    emitIntrinsicPutByValWithThis(generator, m_args->m_listNode, ECMAMode::strict());
    return dst;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getPrototypeOf(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> value = generator.emitNode(node);
    ASSERT(!node->m_next);
    return generator.emitGetPrototypeOf(generator.finalDestination(dst), value.get());
}

static JSPromise::Field promiseInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_promiseFieldFlags)
        return JSPromise::Field::Flags;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_promiseFieldReactionsOrResult)
        return JSPromise::Field::ReactionsOrResult;
    RELEASE_ASSERT_NOT_REACHED();
    return JSPromise::Field::Flags;
}

static JSGenerator::Field generatorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldState)
        return JSGenerator::Field::State;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldNext)
        return JSGenerator::Field::Next;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldThis)
        return JSGenerator::Field::This;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldFrame)
        return JSGenerator::Field::Frame;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldContext)
        return JSGenerator::Field::Context;
    RELEASE_ASSERT_NOT_REACHED();
    return JSGenerator::Field::State;
}

static JSIteratorHelper::Field iteratorHelperInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_iteratorHelperFieldGenerator)
        return JSIteratorHelper::Field::Generator;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_iteratorHelperFieldUnderlyingIterator)
        return JSIteratorHelper::Field::UnderlyingIterator;
    RELEASE_ASSERT_NOT_REACHED();
    return JSIteratorHelper::Field::Generator;
}

static JSAsyncGenerator::Field asyncGeneratorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldState)
        return JSAsyncGenerator::Field::State;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldNext)
        return JSAsyncGenerator::Field::Next;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldThis)
        return JSAsyncGenerator::Field::This;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_generatorFieldFrame)
        return JSAsyncGenerator::Field::Frame;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_asyncGeneratorFieldSuspendReason)
        return JSAsyncGenerator::Field::SuspendReason;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_asyncGeneratorFieldQueueFirst)
        return JSAsyncGenerator::Field::QueueFirst;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_asyncGeneratorFieldQueueLast)
        return JSAsyncGenerator::Field::QueueLast;
    RELEASE_ASSERT_NOT_REACHED();
    return JSAsyncGenerator::Field::State;
}

static AbstractModuleRecord::Field abstractModuleRecordInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_abstractModuleRecordFieldState)
        return AbstractModuleRecord::Field::State;
    RELEASE_ASSERT_NOT_REACHED();
    return AbstractModuleRecord::Field::State;
}

static JSArrayIterator::Field arrayIteratorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_arrayIteratorFieldIndex)
        return JSArrayIterator::Field::Index;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_arrayIteratorFieldIteratedObject)
        return JSArrayIterator::Field::IteratedObject;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_arrayIteratorFieldKind)
        return JSArrayIterator::Field::Kind;
    RELEASE_ASSERT_NOT_REACHED();
    return JSArrayIterator::Field::Index;
}

static JSStringIterator::Field stringIteratorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_stringIteratorFieldIndex)
        return JSStringIterator::Field::Index;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_stringIteratorFieldIteratedString)
        return JSStringIterator::Field::IteratedString;
    RELEASE_ASSERT_NOT_REACHED();
    return JSStringIterator::Field::Index;
}

static JSMapIterator::Field mapIteratorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_mapIteratorFieldEntry)
        return JSMapIterator::Field::Entry;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_mapIteratorFieldIteratedObject)
        return JSMapIterator::Field::IteratedObject;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_mapIteratorFieldStorage)
        return JSMapIterator::Field::Storage;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_mapIteratorFieldKind)
        return JSMapIterator::Field::Kind;
    RELEASE_ASSERT_NOT_REACHED();
    return JSMapIterator::Field::Entry;
}

static JSSetIterator::Field setIteratorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_setIteratorFieldEntry)
        return JSSetIterator::Field::Entry;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_setIteratorFieldIteratedObject)
        return JSSetIterator::Field::IteratedObject;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_setIteratorFieldStorage)
        return JSSetIterator::Field::Storage;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_setIteratorFieldKind)
        return JSSetIterator::Field::Kind;
    RELEASE_ASSERT_NOT_REACHED();
    return JSSetIterator::Field::Entry;
}

static ProxyObject::Field proxyInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_proxyFieldTarget)
        return ProxyObject::Field::Target;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_proxyFieldHandler)
        return ProxyObject::Field::Handler;
    RELEASE_ASSERT_NOT_REACHED();
    return ProxyObject::Field::Target;
}

static JSAsyncFromSyncIterator::Field asyncFromSyncIteratorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_asyncFromSyncIteratorFieldSyncIterator)
        return JSAsyncFromSyncIterator::Field::SyncIterator;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_asyncFromSyncIteratorFieldNextMethod)
        return JSAsyncFromSyncIterator::Field::NextMethod;
    RELEASE_ASSERT_NOT_REACHED();
    return JSAsyncFromSyncIterator::Field::SyncIterator;
}

static JSWrapForValidIterator::Field wrapForValidIteratorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_wrapForValidIteratorFieldIteratedIterator)
        return JSWrapForValidIterator::Field::IteratedIterator;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_wrapForValidIteratorFieldIteratedNextMethod)
        return JSWrapForValidIterator::Field::IteratedNextMethod;
    RELEASE_ASSERT_NOT_REACHED();
    return JSWrapForValidIterator::Field::IteratedNextMethod;
}

static JSDisposableStack::Field disposableStackInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_disposableStackFieldState)
        return JSDisposableStack::Field::State;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_disposableStackFieldCapability)
        return JSDisposableStack::Field::Capability;
    RELEASE_ASSERT_NOT_REACHED();
    return JSDisposableStack::Field::State;
}

static JSAsyncDisposableStack::Field asyncDisposableStackInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_asyncDisposableStackFieldState)
        return JSAsyncDisposableStack::Field::State;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_asyncDisposableStackFieldCapability)
        return JSAsyncDisposableStack::Field::Capability;
    RELEASE_ASSERT_NOT_REACHED();
    return JSAsyncDisposableStack::Field::State;
}

static JSRegExpStringIterator::Field regExpStringIteratorInternalFieldIndex(BytecodeIntrinsicNode* node)
{
    ASSERT(node->entry().type() == BytecodeIntrinsicRegistry::Type::Emitter);
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_regExpStringIteratorFieldRegExp)
        return JSRegExpStringIterator::Field::RegExp;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_regExpStringIteratorFieldString)
        return JSRegExpStringIterator::Field::String;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_regExpStringIteratorFieldGlobal)
        return JSRegExpStringIterator::Field::Global;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_regExpStringIteratorFieldFullUnicode)
        return JSRegExpStringIterator::Field::FullUnicode;
    if (node->entry().emitter() == &BytecodeIntrinsicNode::emit_intrinsic_regExpStringIteratorFieldDone)
        return JSRegExpStringIterator::Field::Done;
    RELEASE_ASSERT_NOT_REACHED();
    return JSRegExpStringIterator::Field::RegExp;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getPromiseInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(promiseInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSPromise::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getGeneratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(generatorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSGenerator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getIteratorHelperInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(iteratorHelperInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSIteratorHelper::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getProxyInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(proxyInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < ProxyObject::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getAsyncGeneratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(asyncGeneratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSAsyncGenerator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getAbstractModuleRecordInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(abstractModuleRecordInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < AbstractModuleRecord::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getArrayIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(arrayIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSArrayIterator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getStringIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(stringIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSStringIterator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getMapIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(mapIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSMapIterator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getSetIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(setIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSSetIterator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getAsyncFromSyncIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(asyncFromSyncIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSAsyncFromSyncIterator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getWrapForValidIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(wrapForValidIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSWrapForValidIterator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getDisposableStackInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(disposableStackInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSDisposableStack::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getAsyncDisposableStackInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(asyncDisposableStackInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSAsyncDisposableStack::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_getRegExpStringIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(regExpStringIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSRegExpStringIterator::numberOfInternalFields);
    ASSERT(!node->m_next);

    return generator.emitGetInternalField(generator.finalDestination(dst), base.get(), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_argument(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    ASSERT(node->m_expr->isNumber());
    double value = static_cast<NumberNode*>(node->m_expr)->value();
    int32_t index = static_cast<int32_t>(value);
    ASSERT(value == index);
    ASSERT(index >= 0);
    ASSERT(!node->m_next);

    // The body functions of generator and async have different mechanism for arguments.
    ASSERT(generator.parseMode() != SourceParseMode::GeneratorBodyMode);
    ASSERT(!isAsyncFunctionBodyParseMode(generator.parseMode()));

    return generator.emitGetArgument(generator.finalDestination(dst), index);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_argumentCount(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(!m_args->m_listNode);

    return generator.emitArgumentCount(generator.finalDestination(dst));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_arrayPush(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    RefPtr<RegisterID> length = generator.emitGetLength(generator.newTemporary(), base.get());
    return generator.move(dst, generator.emitDirectPutByVal(base.get(), length.get(), value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putByIdDirect(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    ASSERT(node->m_expr->isString());
    const Identifier& ident = static_cast<StringNode*>(node->m_expr)->value();
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitDirectPutById(base.get(), ident, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putByIdDirectPrivate(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    ASSERT(node->m_expr->isString());
    SymbolImpl* symbol = generator.vm().propertyNames->builtinNames().lookUpPrivateName(static_cast<StringNode*>(node->m_expr)->value());
    ASSERT(symbol);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitDirectPutById(base.get(), generator.parserArena().identifierArena().makeIdentifier(generator.vm(), symbol), value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putByValDirect(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> index = generator.emitNodeForProperty(node);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitDirectPutByVal(base.get(), index.get(), value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putPromiseInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(promiseInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSPromise::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putGeneratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(generatorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSGenerator::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putAsyncGeneratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(asyncGeneratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSAsyncGenerator::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putArrayIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(arrayIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSArrayIterator::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putStringIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(stringIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSStringIterator::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putMapIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(mapIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSMapIterator::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putSetIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(setIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSSetIterator::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putRegExpStringIteratorInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(regExpStringIteratorInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSRegExpStringIterator::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putDisposableStackInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(disposableStackInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSDisposableStack::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_putAsyncDisposableStackInternalField(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;
    RELEASE_ASSERT(node->m_expr->isBytecodeIntrinsicNode());
    unsigned index = static_cast<unsigned>(asyncDisposableStackInternalFieldIndex(static_cast<BytecodeIntrinsicNode*>(node->m_expr)));
    ASSERT(index < JSAsyncDisposableStack::numberOfInternalFields);
    node = node->m_next;
    RefPtr<RegisterID> value = generator.emitNode(node);

    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitPutInternalField(base.get(), index, value.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_superSamplerBegin(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(!m_args->m_listNode);
    generator.emitLoad(dst, jsUndefined());
    generator.emitSuperSamplerBegin();

    return dst;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_superSamplerEnd(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(!m_args->m_listNode);
    generator.emitSuperSamplerEnd();
    generator.emitLoad(dst, jsUndefined());

    return dst;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_tailCallForwardArguments(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> function = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> thisRegister = generator.emitNode(node);
    ASSERT(!node->m_next);

    RefPtr<RegisterID> finalDst = generator.finalDestination(dst);
    return generator.emitCallForwardArgumentsInTailPosition(finalDst.get(), function.get(), thisRegister.get(), generator.newTemporary(), 0, divot(), divotStart(), divotEnd(), DebuggableCall::No);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_throwTypeError(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    ASSERT(!node->m_next);
    if (node->m_expr->isString()) {
        const Identifier& ident = static_cast<StringNode*>(node->m_expr)->value();
        generator.emitThrowTypeError(ident);
    } else {
        RefPtr<RegisterID> message = generator.emitNode(node);
        generator.emitThrowStaticError(ErrorTypeWithExtension::TypeError, message.get());
    }
    return dst;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_throwRangeError(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    ASSERT(!node->m_next);
    if (node->m_expr->isString()) {
        const Identifier& ident = static_cast<StringNode*>(node->m_expr)->value();
        generator.emitThrowRangeError(ident);
    } else {
        RefPtr<RegisterID> message = generator.emitNode(node);
        generator.emitThrowStaticError(ErrorTypeWithExtension::RangeError, message.get());
    }

    return dst;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_throwOutOfMemoryError(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(!m_args->m_listNode);

    generator.emitThrowOutOfMemoryError();
    return dst;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_tryGetById(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;

    ASSERT(node->m_expr->isString());
    const Identifier& ident = static_cast<StringNode*>(node->m_expr)->value();
    ASSERT(!node->m_next);

    RefPtr<RegisterID> finalDest = generator.finalDestination(dst);
    return generator.emitTryGetById(finalDest.get(), base.get(), ident);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_tryGetByIdWithWellKnownSymbol(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> base = generator.emitNode(node);
    node = node->m_next;

    ASSERT(node->m_expr->isString());
    SymbolImpl* symbol = generator.vm().propertyNames->builtinNames().lookUpWellKnownSymbol(static_cast<StringNode*>(node->m_expr)->value());
    RELEASE_ASSERT(symbol);
    ASSERT(!node->m_next);

    RefPtr<RegisterID> finalDest = generator.finalDestination(dst);
    return generator.emitTryGetById(finalDest.get(), base.get(), generator.parserArena().identifierArena().makeIdentifier(generator.vm(), symbol));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_toNumber(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> src = generator.emitNode(node);
    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitToNumber(generator.tempDestination(dst), src.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_toString(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> src = generator.emitNode(node);
    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitToString(generator.tempDestination(dst), src.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_toPropertyKey(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> src = generator.emitNode(node);
    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitToPropertyKey(generator.tempDestination(dst), src.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_toObject(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> src = generator.emitNode(node);
    node = node->m_next;

    RefPtr<RegisterID> temp = generator.tempDestination(dst);
    if (node) {
        ASSERT(node->m_expr->isString());
        const Identifier& message = static_cast<StringNode*>(node->m_expr)->value();
        ASSERT(!node->m_next);
        return generator.move(dst, generator.emitToObject(temp.get(), src.get(), message));
    }
    return generator.move(dst, generator.emitToObject(temp.get(), src.get(), generator.vm().propertyNames->emptyIdentifier));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_toThis(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> src = generator.emitNode(node);
    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitToThis(src.get()));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_idWithProfile(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> idValue = generator.newTemporary();
    generator.emitNode(idValue.get(), node);
    SpeculatedType speculation = SpecNone;
    while (node->m_next) {
        node = node->m_next;
        ASSERT(node->m_expr->isString());
        const Identifier& ident = static_cast<StringNode*>(node->m_expr)->value();
        speculation |= speculationFromString(ident.utf8().data());
    }

    return generator.move(dst, generator.emitIdWithProfile(idValue.get(), speculation));
}

#define CREATE_INTRINSIC_FOR_BRAND_CHECK(lowerName, upperName) \
    RegisterID* BytecodeIntrinsicNode::emit_intrinsic_##lowerName(JSC::BytecodeGenerator& generator, JSC::RegisterID* dst) \
    {                                                                                                                      \
        ArgumentListNode* node = m_args->m_listNode;                                                                       \
        RefPtr<RegisterID> src = generator.emitNode(node);                                                                 \
        ASSERT(!node->m_next);                                                                                             \
        return generator.move(dst, generator.emit##upperName(generator.tempDestination(dst), src.get()));                  \
    }

CREATE_INTRINSIC_FOR_BRAND_CHECK(isObject, IsObject)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isCallable, IsCallable)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isConstructor, IsConstructor)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isJSArray, IsJSArray)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isProxyObject, IsProxyObject)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isDerivedArray, IsDerivedArray)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isGenerator, IsGenerator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isIteratorHelper, IsIteratorHelper)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isAsyncGenerator, IsAsyncGenerator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isPromise, IsPromise)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isRegExpObject, IsRegExpObject)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isMap, IsMap)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isSet, IsSet)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isShadowRealm, IsShadowRealm)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isStringIterator, IsStringIterator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isArrayIterator, IsArrayIterator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isMapIterator, IsMapIterator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isSetIterator, IsSetIterator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isUndefinedOrNull, IsUndefinedOrNull)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isWrapForValidIterator, IsWrapForValidIterator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isRegExpStringIterator, IsRegExpStringIterator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isAsyncFromSyncIterator, IsAsyncFromSyncIterator)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isDisposableStack, IsDisposableStack)
CREATE_INTRINSIC_FOR_BRAND_CHECK(isAsyncDisposableStack, IsAsyncDisposableStack)

#undef CREATE_INTRINSIC_FOR_BRAND_CHECK

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_mustValidateResultOfProxyGetAndSetTraps(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> src = generator.emitNode(node);
    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitHasStructureWithFlags(generator.tempDestination(dst), src.get(), Structure::s_hasNonConfigurableReadOnlyOrGetterSetterPropertiesBits));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_mustValidateResultOfProxyTrapsExceptGetAndSet(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> src = generator.emitNode(node);
    ASSERT(!node->m_next);

    return generator.move(dst, generator.emitHasStructureWithFlags(generator.tempDestination(dst), src.get(), Structure::s_hasNonConfigurablePropertiesBits | Structure::s_didPreventExtensionsBits));
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_newArrayWithSize(JSC::BytecodeGenerator& generator, JSC::RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> size = generator.emitNode(node);
    ASSERT(!node->m_next);

    RefPtr<RegisterID> finalDestination = generator.finalDestination(dst);
    generator.emitNewArrayWithSize(finalDestination.get(), size.get());
    return finalDestination.get();
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_newArrayWithSpecies(JSC::BytecodeGenerator& generator, JSC::RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> size = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> array = generator.emitNode(node);
    ASSERT(!node->m_next);

    RefPtr<RegisterID> finalDestination = generator.finalDestination(dst);
    generator.emitNewArrayWithSpecies(finalDestination.get(), size.get(), array.get());
    return finalDestination.get();
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_createPromise(JSC::BytecodeGenerator& generator, JSC::RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> newTarget = generator.emitNode(node);
    node = node->m_next;
    bool isInternalPromise = static_cast<BooleanNode*>(node->m_expr)->value();
    ASSERT(!node->m_next);

    return generator.emitCreatePromise(generator.finalDestination(dst), newTarget.get(), isInternalPromise);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_newPromise(JSC::BytecodeGenerator& generator, JSC::RegisterID* dst)
{
    ASSERT(!m_args->m_listNode);
    RefPtr<RegisterID> finalDestination = generator.finalDestination(dst);
    bool isInternalPromise = false;
    generator.emitNewPromise(finalDestination.get(), isInternalPromise);
    return finalDestination.get();
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_iteratorGenericClose(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> iterator = generator.emitNode(node);
    ASSERT(!node->m_next);

    generator.emitIteratorGenericClose(iterator.get(), this);
    return dst;
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_iteratorGenericNext(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> nextMethod = generator.emitNode(node);
    node = node->m_next;
    RefPtr<RegisterID> iterator = generator.emitNode(node);
    ASSERT(!node->m_next);

    return generator.emitIteratorGenericNext(generator.finalDestination(dst), nextMethod.get(), iterator.get(), this);
}

RegisterID* BytecodeIntrinsicNode::emit_intrinsic_ifAbruptCloseIterator(BytecodeGenerator& generator, RegisterID* dst)
{
    ArgumentListNode* node = m_args->m_listNode;
    RefPtr<RegisterID> iterator = generator.emitNode(node);
    node = node->m_next;
    ASSERT(!node->m_next);

    Ref<Label> end = generator.newLabel();
    auto emitSecondArgumentNode = scopedLambda<void(BytecodeGenerator&)>([&](BytecodeGenerator& generator) {
        generator.emitNode(node);
        generator.emitJump(end.get());
    });

    auto emitIteratorClose = scopedLambda<void(BytecodeGenerator&)>([&](BytecodeGenerator& generator) {
        generator.emitIteratorGenericClose(iterator.get(), this);
    });

    generator.emitTryWithFinallyThatDoesNotShadowException(emitSecondArgumentNode, emitIteratorClose);
    generator.emitLabel(end.get());

    return dst;
}

#define JSC_DECLARE_BYTECODE_INTRINSIC_CONSTANT_GENERATORS(name) \
    RegisterID* BytecodeIntrinsicNode::emit_intrinsic_##name(BytecodeGenerator& generator, RegisterID* dst) \
    { \
        ASSERT(!m_args); \
        ASSERT(type() == Type::Constant); \
        if (dst == generator.ignoredResult()) \
            return nullptr; \
        return generator.emitLoad(dst, generator.vm().bytecodeIntrinsicRegistry().name##Value(generator)); \
    }
    JSC_COMMON_BYTECODE_INTRINSIC_CONSTANTS_EACH_NAME(JSC_DECLARE_BYTECODE_INTRINSIC_CONSTANT_GENERATORS)
#undef JSC_DECLARE_BYTECODE_INTRINSIC_CONSTANT_GENERATORS

// ------------------------------ FunctionCallBracketNode ----------------------------------

RegisterID* FunctionCallBracketNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> function = generator.tempDestination(dst);
    RefPtr<RegisterID> returnValue = generator.finalDestination(dst, function.get());
    bool baseIsSuper = m_base->isSuperNode();
    bool subscriptIsNonIndexString = isNonIndexStringElement(*m_subscript);

    RefPtr<RegisterID> base;
    if (baseIsSuper)
        base = emitSuperBaseForCallee(generator);
    else {
        if (subscriptIsNonIndexString)
            base = generator.emitNode(m_base);
        else
            base = generator.emitNodeForLeftHandSide(m_base, m_subscriptHasAssignments, m_subscript->isPure(generator));

        if (m_base->isOptionalChainBase())
            generator.emitOptionalCheck(base.get());
    }

    RefPtr<RegisterID> thisRegister;
    if (baseIsSuper) {
        // Note that we only need to do this once because we either have a non-TDZ this or we throw. Once we have a non-TDZ this, we can't change its value back to TDZ.
        thisRegister = generator.ensureThis();
    }
    if (subscriptIsNonIndexString) {
        generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());
        if (baseIsSuper)
            generator.emitGetById(function.get(), base.get(), thisRegister.get(), static_cast<StringNode*>(m_subscript)->value());
        else
            generator.emitGetById(function.get(), base.get(), static_cast<StringNode*>(m_subscript)->value());
    } else {
        RefPtr<RegisterID> property = generator.emitNodeForProperty(m_subscript);
        generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());
        if (baseIsSuper)
            generator.emitGetByVal(function.get(), base.get(), thisRegister.get(), property.get());
        else
            generator.emitGetByVal(function.get(), base.get(), property.get());
    }
    if (isOptionalCall())
        generator.emitOptionalCheck(function.get());

    CallArguments callArguments(generator, m_args);
    if (baseIsSuper) {
        generator.emitTDZCheck(generator.thisRegister());
        generator.move(callArguments.thisRegister(), thisRegister.get());
    } else
        generator.move(callArguments.thisRegister(), base.get());
    RegisterID* ret = generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
    generator.emitProfileType(returnValue.get(), divotStart(), divotEnd());
    return ret;
}

// ------------------------------ FunctionCallDotNode ----------------------------------

RegisterID* FunctionCallDotNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> function = generator.tempDestination(dst);
    RefPtr<RegisterID> returnValue = generator.finalDestination(dst, function.get());
    CallArguments callArguments(generator, m_args);
    bool baseIsSuper = m_base->isSuperNode();
    bool shouldGetArgumentsDotLengthFast = generator.shouldGetArgumentsDotLengthFast(this);
    if (baseIsSuper)
        generator.move(callArguments.thisRegister(), generator.ensureThis());
    else if (shouldGetArgumentsDotLengthFast)
        generator.emitLoad(callArguments.thisRegister(), jsUndefined());
    else {
        generator.emitNode(callArguments.thisRegister(), m_base);
        if (m_base->isOptionalChainBase())
            generator.emitOptionalCheck(callArguments.thisRegister());
    }
    generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());

    if (shouldGetArgumentsDotLengthFast)
        generator.emitArgumentCount(function.get());
    else {
        RefPtr<RegisterID> base = baseIsSuper ? emitSuperBaseForCallee(generator) : callArguments.thisRegister();
        emitGetPropertyValue(generator, function.get(), base.get());
    }

    if (isOptionalCall())
        generator.emitOptionalCheck(function.get());

    RegisterID* ret = generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
    generator.emitProfileType(returnValue.get(), divotStart(), divotEnd());
    return ret;
}

static constexpr size_t maxDistanceToInnermostCallOrApply = 2;

RegisterID* CallFunctionCallDotNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> returnValue = generator.finalDestination(dst);
    RefPtr<RegisterID> base = generator.emitNode(m_base);

    if (m_base->isOptionalChainBase())
        generator.emitOptionalCheck(base.get());

    generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());

    RefPtr<RegisterID> function;
    auto makeFunction = [&] {
        if (m_base->isSuperNode()) {
            RefPtr<RegisterID> thisValue = generator.ensureThis();
            function = generator.emitGetById(generator.tempDestination(dst), base.get(), thisValue.get(), generator.propertyNames().builtinNames().callPublicName());
        } else
            function = generator.emitGetById(generator.tempDestination(dst), base.get(), generator.propertyNames().builtinNames().callPublicName());

        if (isOptionalCall())
            generator.emitOptionalCheck(function.get());
    };

    bool emitCallCheck = !generator.isBuiltinFunction();
    if (m_distanceToInnermostCallOrApply > maxDistanceToInnermostCallOrApply && emitCallCheck) {
        makeFunction();
        CallArguments callArguments(generator, m_args);
        generator.move(callArguments.thisRegister(), base.get());
        generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        generator.move(dst, returnValue.get());
        return returnValue.get();
    }

    Ref<Label> realCall = generator.newLabel();
    Ref<Label> end = generator.newLabel();

    if (emitCallCheck) {
        makeFunction();
        generator.emitJumpIfNotFunctionCall(function.get(), realCall.get());
    }
    {
        if (m_args->m_listNode && m_args->m_listNode->m_expr && m_args->m_listNode->m_expr->isSpreadExpression()) {
            SpreadExpressionNode* spread = static_cast<SpreadExpressionNode*>(m_args->m_listNode->m_expr);
            ExpressionNode* subject = spread->expression();
            RefPtr<RegisterID> argumentsRegister;
            argumentsRegister = generator.emitNode(subject);
            generator.emitExpressionInfo(spread->divot(), spread->divotStart(), spread->divotEnd());
            RefPtr<RegisterID> thisRegister = generator.emitGetByVal(generator.newTemporary(), argumentsRegister.get(), generator.emitLoad(nullptr, jsNumber(0)));
            generator.emitCallVarargsInTailPosition(returnValue.get(), base.get(), thisRegister.get(), argumentsRegister.get(), generator.newTemporary(), 1, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        } else if (m_args->m_listNode && m_args->m_listNode->m_expr) {
            ArgumentListNode* oldList = m_args->m_listNode;
            m_args->m_listNode = m_args->m_listNode->m_next;

            RefPtr<RegisterID> realFunction = generator.move(generator.tempDestination(dst), base.get());
            CallArguments callArguments(generator, m_args);
            generator.emitNode(callArguments.thisRegister(), oldList->m_expr);
            generator.emitCallInTailPosition(returnValue.get(), realFunction.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
            m_args->m_listNode = oldList;
        } else {
            RefPtr<RegisterID> realFunction = generator.move(generator.tempDestination(dst), base.get());
            CallArguments callArguments(generator, m_args);
            generator.emitLoad(callArguments.thisRegister(), jsUndefined());
            generator.emitCallInTailPosition(returnValue.get(), realFunction.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        }
    }
    if (emitCallCheck) {
        generator.emitJump(end.get());
        generator.emitLabel(realCall.get());
        {
            CallArguments callArguments(generator, m_args);
            generator.move(callArguments.thisRegister(), base.get());
            generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        }
        generator.emitLabel(end.get());
    }
    generator.emitProfileType(returnValue.get(), divotStart(), divotEnd());
    return returnValue.get();
}

RegisterID* HasOwnPropertyFunctionCallDotNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> returnValue = generator.finalDestination(dst);
    RefPtr<RegisterID> base = generator.emitNode(m_base);

    if (m_base->isOptionalChainBase())
        generator.emitOptionalCheck(base.get());

    generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());

    RefPtr<RegisterID> function = generator.emitGetById(generator.newTemporary(), base.get(), generator.propertyNames().hasOwnProperty);
    if (isOptionalCall())
        generator.emitOptionalCheck(function.get());

    RELEASE_ASSERT(m_args->m_listNode && m_args->m_listNode->m_expr && !m_args->m_listNode->m_next);  
    ExpressionNode* argument = m_args->m_listNode->m_expr;
    RELEASE_ASSERT(argument->isResolveNode());
    ForInContext* context = nullptr;
    Variable argumentVariable = generator.variable(static_cast<ResolveNode*>(argument)->identifier());
    if (argumentVariable.isLocal()) {
        RegisterID* property = argumentVariable.local();
        context = generator.findForInContext(property);
    }

    auto canUseFastHasOwnProperty = [&] {
        if (!context)
            return false;
        if (!context->baseVariable())
            return false;
        if (m_base->isResolveNode())
            return generator.variable(static_cast<ResolveNode*>(m_base)->identifier()) == context->baseVariable().value();
        if (m_base->isThisNode()) {
            // After generator.ensureThis (which must be invoked in |base|'s materialization), we can ensure that |this| is in local this-register.
            ASSERT(base);
            return generator.variable(generator.propertyNames().builtinNames().thisPrivateName(), ThisResolutionType::Local) == context->baseVariable().value();
        }
        return false;
    };

    if (canUseFastHasOwnProperty()) {
        // It is possible that base register is variable and each for-in body replaces JS object in the base register with a different one.
        // Even though, this is OK since HasOwnStructureProperty will reject the replaced JS object.
        Ref<Label> realCall = generator.newLabel();
        Ref<Label> end = generator.newLabel();

        unsigned branchInsnOffset = generator.emitWideJumpIfNotFunctionHasOwnProperty(function.get(), realCall.get());
        generator.emitEnumeratorHasOwnProperty(returnValue.get(), base.get(), context->mode(), generator.emitNode(argument), context->propertyOffset(), context->enumerator());
        generator.emitJump(end.get());

        generator.emitLabel(realCall.get());
        {
            CallArguments callArguments(generator, m_args);
            generator.move(callArguments.thisRegister(), base.get());
            generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        }

        generator.emitLabel(end.get());

        generator.recordHasOwnPropertyInForInLoop(*context, branchInsnOffset, realCall);
    } else {
        CallArguments callArguments(generator, m_args);
        generator.move(callArguments.thisRegister(), base.get());
        generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
    }

    generator.emitProfileType(returnValue.get(), divotStart(), divotEnd());
    return returnValue.get();
}

static bool areTrivialApplyArguments(ArgumentsNode* args)
{
    return !args->m_listNode || !args->m_listNode->m_expr || !args->m_listNode->m_next
        || (!args->m_listNode->m_next->m_next && args->m_listNode->m_next->m_expr->isSimpleArray());
}

RegisterID* ApplyFunctionCallDotNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    // A few simple cases can be trivially handled as ordinary function calls.
    // function.apply(), function.apply(arg) -> identical to function.call
    // function.apply(thisArg, [arg0, arg1, ...]) -> can be trivially coerced into function.call(thisArg, arg0, arg1, ...) and saves object allocation
    bool mayBeCall = areTrivialApplyArguments(m_args);

    RefPtr<RegisterID> returnValue = generator.finalDestination(dst);
    RefPtr<RegisterID> base = generator.emitNode(m_base);

    if (m_base->isOptionalChainBase())
        generator.emitOptionalCheck(base.get());

    RefPtr<RegisterID> function;
    auto makeFunction = [&] {
        if (m_base->isSuperNode()) {
            RefPtr<RegisterID> thisValue = generator.ensureThis();
            function = generator.emitGetById(generator.tempDestination(dst), base.get(), thisValue.get(), generator.propertyNames().builtinNames().applyPublicName());
        } else
            function = generator.emitGetById(generator.tempDestination(dst), base.get(), generator.propertyNames().builtinNames().applyPublicName());

        if (isOptionalCall())
            generator.emitOptionalCheck(function.get());
    };

    bool emitCallCheck = !generator.isBuiltinFunction();
    if (m_distanceToInnermostCallOrApply > maxDistanceToInnermostCallOrApply && emitCallCheck) {
        makeFunction();
        CallArguments callArguments(generator, m_args);
        generator.move(callArguments.thisRegister(), base.get());
        generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        generator.move(dst, returnValue.get());
        return returnValue.get();
    }

    Ref<Label> realCall = generator.newLabel();
    Ref<Label> end = generator.newLabel();
    generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());
    if (emitCallCheck) {
        makeFunction();
        ASSERT(!m_base->isResolveNode() || static_cast<ResolveNode*>(m_base)->identifier() != "Reflect"_s);
        generator.emitJumpIfNotFunctionApply(function.get(), realCall.get());
    }
    if (mayBeCall) {
        if (m_args->m_listNode && m_args->m_listNode->m_expr) {
            ArgumentListNode* oldList = m_args->m_listNode;
            if (m_args->m_listNode->m_expr->isSpreadExpression()) {
                SpreadExpressionNode* spread = static_cast<SpreadExpressionNode*>(m_args->m_listNode->m_expr);
                RefPtr<RegisterID> realFunction = generator.move(generator.newTemporary(), base.get());
                RefPtr<RegisterID> index = generator.emitLoad(generator.newTemporary(), jsNumber(0));
                RefPtr<RegisterID> thisRegister = generator.emitLoad(generator.newTemporary(), jsUndefined());
                RefPtr<RegisterID> argumentsRegister = generator.emitLoad(generator.newTemporary(), jsUndefined());
                
                auto extractor = scopedLambda<void(BytecodeGenerator&, RegisterID*)>([&thisRegister, &argumentsRegister, &index](BytecodeGenerator& generator, RegisterID* value)
                {
                    Ref<Label> haveThis = generator.newLabel();
                    Ref<Label> end = generator.newLabel();
                    generator.emitJumpIfFalse(generator.emitEqualityOp<OpStricteq>(generator.newTemporary(), index.get(), generator.emitLoad(nullptr, jsNumber(0))), haveThis.get());
                    generator.move(thisRegister.get(), value);
                    generator.emitLoad(index.get(), jsNumber(1));
                    generator.emitJump(end.get());
                    generator.emitLabel(haveThis.get());
                    generator.emitJumpIfFalse(generator.emitEqualityOp<OpStricteq>(generator.newTemporary(), index.get(), generator.emitLoad(nullptr, jsNumber(1))), end.get());
                    generator.move(argumentsRegister.get(), value);
                    generator.emitLoad(index.get(), jsNumber(2));
                    generator.emitLabel(end.get());
                });
                generator.emitEnumeration(this, spread->expression(), extractor);
                generator.emitCallVarargsInTailPosition(returnValue.get(), realFunction.get(), thisRegister.get(), argumentsRegister.get(), generator.newTemporary(), 0, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
            } else if (m_args->m_listNode->m_next) {
                ASSERT(m_args->m_listNode->m_next->m_expr->isSimpleArray());
                ASSERT(!m_args->m_listNode->m_next->m_next);
                m_args->m_listNode = static_cast<ArrayNode*>(m_args->m_listNode->m_next->m_expr)->toArgumentList(generator.parserArena(), 0, 0);
                RefPtr<RegisterID> realFunction = generator.move(generator.tempDestination(dst), base.get());
                CallArguments callArguments(generator, m_args);
                generator.emitNode(callArguments.thisRegister(), oldList->m_expr);
                generator.emitCallInTailPosition(returnValue.get(), realFunction.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
            } else {
                m_args->m_listNode = m_args->m_listNode->m_next;
                RefPtr<RegisterID> realFunction = generator.move(generator.tempDestination(dst), base.get());
                CallArguments callArguments(generator, m_args);
                generator.emitNode(callArguments.thisRegister(), oldList->m_expr);
                generator.emitCallInTailPosition(returnValue.get(), realFunction.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
            }
            m_args->m_listNode = oldList;
        } else {
            RefPtr<RegisterID> realFunction = generator.move(generator.tempDestination(dst), base.get());
            CallArguments callArguments(generator, m_args);
            generator.emitLoad(callArguments.thisRegister(), jsUndefined());
            generator.emitCallInTailPosition(returnValue.get(), realFunction.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        }
    } else {
        ASSERT(m_args->m_listNode && m_args->m_listNode->m_next);
        RefPtr<RegisterID> realFunction = generator.move(generator.tempDestination(dst), base.get());
        RefPtr<RegisterID> thisRegister = generator.emitNode(m_args->m_listNode->m_expr);
        RefPtr<RegisterID> argsRegister;
        ArgumentListNode* args = m_args->m_listNode->m_next;
        argsRegister = generator.emitNode(args->m_expr);

        // Function.prototype.apply ignores extra arguments, but we still
        // need to evaluate them for side effects.
        while ((args = args->m_next))
            generator.emitNode(args->m_expr);

        generator.emitCallVarargsInTailPosition(returnValue.get(), realFunction.get(), thisRegister.get(), argsRegister.get(), generator.newTemporary(), 0, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
    }
    if (emitCallCheck) {
        generator.emitJump(end.get());
        generator.emitLabel(realCall.get());
        CallArguments callArguments(generator, m_args);
        generator.move(callArguments.thisRegister(), base.get());
        generator.emitCallInTailPosition(returnValue.get(), function.get(), NoExpectedFunction, callArguments, divot(), divotStart(), divotEnd(), DebuggableCall::Yes);
        generator.emitLabel(end.get());
    }
    generator.emitProfileType(returnValue.get(), divotStart(), divotEnd());
    return returnValue.get();
}

// ------------------------------ PostfixNode ----------------------------------

static RegisterID* emitIncOrDec(BytecodeGenerator& generator, RegisterID* srcDst, Operator oper)
{
    return (oper == Operator::PlusPlus) ? generator.emitInc(srcDst) : generator.emitDec(srcDst);
}

static RegisterID* emitPostIncOrDec(BytecodeGenerator& generator, RegisterID* dst, RegisterID* srcDst, Operator oper)
{
    if (dst == srcDst)
        return generator.emitToNumeric(generator.finalDestination(dst), srcDst);
    RefPtr<RegisterID> tmp = generator.emitToNumeric(generator.newTemporary(), srcDst);
    RefPtr<RegisterID> result = generator.tempDestination(srcDst);
    generator.move(result.get(), tmp.get());
    emitIncOrDec(generator, result.get(), oper);
    generator.move(srcDst, result.get());
    return generator.move(dst, tmp.get());
}

RegisterID* PostfixNode::emitResolve(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        return PrefixNode::emitResolve(generator, dst);

    ASSERT(m_expr->isResolveNode());
    ResolveNode* resolve = static_cast<ResolveNode*>(m_expr);
    const Identifier& ident = resolve->identifier();

    Variable var = generator.variable(ident);
    if (RegisterID* local = var.local()) {
        generator.emitTDZCheckIfNecessary(var, local, nullptr);
        RefPtr<RegisterID> localReg = local;
        if (var.isReadOnly()) {
            generator.emitReadOnlyExceptionIfNeeded(var);
            localReg = generator.move(generator.tempDestination(dst), local);
        }
        RefPtr<RegisterID> oldValue = emitPostIncOrDec(generator, generator.finalDestination(dst), localReg.get(), m_operator);
        generator.emitProfileType(localReg.get(), var, divotStart(), divotEnd());
        return oldValue.get();
    }

    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
    RefPtr<RegisterID> value = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
    generator.emitTDZCheckIfNecessary(var, value.get(), nullptr);
    if (var.isReadOnly()) {
        bool threwException = generator.emitReadOnlyExceptionIfNeeded(var);
        if (threwException)
            return value.get();
    }
    RefPtr<RegisterID> oldValue = emitPostIncOrDec(generator, generator.finalDestination(dst), value.get(), m_operator);
    if (!var.isReadOnly()) {
        generator.emitPutToScope(scope.get(), var, value.get(), generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, InitializationMode::NotInitialization);
        generator.emitProfileType(value.get(), var, divotStart(), divotEnd());
    }

    return oldValue.get();
}

RegisterID* PostfixNode::emitBracket(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        return PrefixNode::emitBracket(generator, dst);

    ASSERT(m_expr->isBracketAccessorNode());
    BracketAccessorNode* bracketAccessor = static_cast<BracketAccessorNode*>(m_expr);
    ExpressionNode* baseNode = bracketAccessor->base();
    ExpressionNode* subscript = bracketAccessor->subscript();

    RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(baseNode, bracketAccessor->subscriptHasAssignments(), subscript->isPure(generator));
    RefPtr<RegisterID> property = generator.emitNodeForProperty(subscript);
    if (!subscript->isNumber() && !subscript->isString()) {
        // Never double-evaluate the subscript expression;
        // don't even evaluate it once if the base isn't subscriptable.
        generator.emitRequireObjectCoercible(base.get(), "Cannot access property of undefined or null"_s);
        property = generator.emitToPropertyKeyOrNumber(generator.newTemporary(), property.get());
    }

    generator.emitExpressionInfo(bracketAccessor->divot(), bracketAccessor->divotStart(), bracketAccessor->divotEnd());
    RefPtr<RegisterID> value;
    RefPtr<RegisterID> thisValue;
    if (baseNode->isSuperNode()) {
        thisValue = generator.ensureThis();
        value = generator.emitGetByVal(generator.newTemporary(), base.get(), thisValue.get(), property.get());
    } else
        value = generator.emitGetByVal(generator.newTemporary(), base.get(), property.get());
    RegisterID* oldValue = emitPostIncOrDec(generator, generator.tempDestination(dst), value.get(), m_operator);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    if (baseNode->isSuperNode())
        generator.emitPutByVal(base.get(), thisValue.get(), property.get(), value.get());
    else
        generator.emitPutByVal(base.get(), property.get(), value.get());
    generator.emitProfileType(value.get(), divotStart(), divotEnd());
    return generator.move(dst, oldValue);
}

RegisterID* PostfixNode::emitDot(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        return PrefixNode::emitDot(generator, dst);

    ASSERT(m_expr->isDotAccessorNode());
    DotAccessorNode* dotAccessor = static_cast<DotAccessorNode*>(m_expr);
    ExpressionNode* baseNode = dotAccessor->base();
    bool baseIsSuper = baseNode->isSuperNode();
    const Identifier& ident = dotAccessor->identifier();

    RefPtr<RegisterID> base = generator.emitNode(baseNode);

    generator.emitExpressionInfo(dotAccessor->divot(), dotAccessor->divotStart(), dotAccessor->divotEnd());

    if (dotAccessor->isPrivateMember()) {
        ASSERT(!baseIsSuper);
        auto privateTraits = generator.getPrivateTraits(ident);

        if (privateTraits.isField()) {
            Variable var = generator.variable(ident);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            ASSERT(scope); // Private names are always captured.
            RefPtr<RegisterID> privateName = generator.newTemporary();
            generator.emitGetFromScope(privateName.get(), scope.get(), var, DoNotThrowIfNotFound);

            RefPtr<RegisterID> value = generator.emitGetPrivateName(generator.newTemporary(), base.get(), privateName.get());
            RefPtr<RegisterID> oldValue = emitPostIncOrDec(generator, generator.tempDestination(dst), value.get(), m_operator);
            generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
            generator.emitPrivateFieldPut(base.get(), privateName.get(), value.get());
            generator.emitProfileType(value.get(), divotStart(), divotEnd());
            return generator.move(dst, oldValue.get());
        }

        if (privateTraits.isMethod()) {
            Variable var = generator.variable(ident);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            ASSERT(scope); // Private names are always captured.
            RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
            generator.emitCheckPrivateBrand(base.get(), privateBrandSymbol.get(), privateTraits.isStatic());

            generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
            generator.emitThrowTypeError("Trying to access an undefined private setter"_s);
            return generator.tempDestination(dst);
        }

        Variable var = generator.variable(ident);
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        ASSERT(scope); // Private names are always captured.
        RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
        generator.emitCheckPrivateBrand(base.get(), privateBrandSymbol.get(), privateTraits.isStatic());

        RefPtr<RegisterID> value;
        if (privateTraits.isGetter()) {
            RefPtr<RegisterID> getterSetterObj = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
            RefPtr<RegisterID> getterFunction = generator.emitDirectGetById(generator.newTemporary(), getterSetterObj.get(), generator.propertyNames().builtinNames().getPrivateName());
            CallArguments args(generator, nullptr);
            generator.move(args.thisRegister(), base.get());
            value = generator.emitCall(generator.newTemporary(), getterFunction.get(), NoExpectedFunction, args, m_position, m_position, m_position, DebuggableCall::Yes);
        } else {
            generator.emitThrowTypeError("Trying to access an undefined private getter"_s);
            return generator.tempDestination(dst);
        }

        RefPtr<RegisterID> oldValue = emitPostIncOrDec(generator, generator.tempDestination(dst), value.get(), m_operator);
        generator.emitExpressionInfo(divot(), divotStart(), divotEnd());

        if (privateTraits.isSetter()) {
            RefPtr<RegisterID> getterSetterObj = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
            RefPtr<RegisterID> setterFunction = generator.emitDirectGetById(generator.newTemporary(), getterSetterObj.get(), generator.propertyNames().builtinNames().setPrivateName());
            CallArguments args(generator, nullptr, 1);
            generator.move(args.thisRegister(), base.get());
            generator.move(args.argumentRegister(0), value.get());
            generator.emitCallIgnoreResult(generator.newTemporary(), setterFunction.get(), NoExpectedFunction, args, m_position, m_position, m_position, DebuggableCall::Yes);
            generator.emitProfileType(value.get(), divotStart(), divotEnd());
            return generator.move(dst, oldValue.get());
        } 

        generator.emitThrowTypeError("Trying to access an undefined private getter"_s);
        return generator.move(dst, oldValue.get());
    }

    RefPtr<RegisterID> value;
    RefPtr<RegisterID> thisValue;
    if (baseIsSuper) {
        thisValue = generator.ensureThis();
        value = generator.emitGetById(generator.newTemporary(), base.get(), thisValue.get(), ident);
    } else
        value = generator.emitGetById(generator.newTemporary(), base.get(), ident);
    RegisterID* oldValue = emitPostIncOrDec(generator, generator.tempDestination(dst), value.get(), m_operator);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    if (baseIsSuper)
        generator.emitPutById(base.get(), thisValue.get(), ident, value.get());
    else
        generator.emitPutById(base.get(), ident, value.get());
    generator.emitProfileType(value.get(), divotStart(), divotEnd());
    return generator.move(dst, oldValue);
}

RegisterID* PostfixNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (m_expr->isResolveNode())
        return emitResolve(generator, dst);

    if (m_expr->isBracketAccessorNode())
        return emitBracket(generator, dst);

    if (m_expr->isDotAccessorNode())
        return emitDot(generator, dst);

    ASSERT(m_expr->isFunctionCall());
    generator.emitNode(m_expr);
    return emitThrowReferenceError(generator, m_operator == Operator::PlusPlus
        ? "Postfix ++ operator applied to value that is not a reference."_s
        : "Postfix -- operator applied to value that is not a reference."_s,
        dst);
}

// ------------------------------ DeleteResolveNode -----------------------------------

RegisterID* DeleteResolveNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    Variable var = generator.variable(m_ident);
    if (var.local())
        return generator.emitLoad(generator.finalDestination(dst), false);

    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RefPtr<RegisterID> base = generator.emitResolveScope(dst, var);
    return generator.emitDeleteById(generator.finalDestination(dst, base.get()), base.get(), m_ident);
}

// ------------------------------ DeleteBracketNode -----------------------------------

RegisterID* DeleteBracketNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> finalDest = generator.finalDestination(dst);
    RefPtr<RegisterID> r0 = generator.emitNode(m_base);

    if (m_base->isOptionalChainBase())
        generator.emitOptionalCheck(r0.get());

    RefPtr<RegisterID> r1 = generator.emitNode(m_subscript);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    if (m_base->isSuperNode())
        return emitThrowReferenceError(generator, "Cannot delete a super property"_s, dst);
    return generator.emitDeleteByVal(finalDest.get(), r0.get(), r1.get());
}

// ------------------------------ DeleteDotNode -----------------------------------

RegisterID* DeleteDotNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> finalDest = generator.finalDestination(dst);
    RefPtr<RegisterID> r0 = generator.emitNode(m_base);

    if (m_base->isOptionalChainBase())
        generator.emitOptionalCheck(r0.get());

    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    if (m_base->isSuperNode())
        return emitThrowReferenceError(generator, "Cannot delete a super property"_s, dst);
    return generator.emitDeleteById(finalDest.get(), r0.get(), m_ident);
}

// ------------------------------ DeleteValueNode -----------------------------------

RegisterID* DeleteValueNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    generator.emitNodeInIgnoreResultPosition(m_expr);

    // delete on a non-location expression ignores the value and returns true
    return generator.emitLoad(generator.finalDestination(dst), true);
}

// ------------------------------ VoidNode -------------------------------------

RegisterID* VoidNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult()) {
        generator.emitNodeInIgnoreResultPosition(m_expr);
        return nullptr;
    }
    RefPtr<RegisterID> r0 = generator.emitNode(m_expr);
    return generator.emitLoad(dst, jsUndefined());
}

// ------------------------------ TypeOfResolveNode -----------------------------------

RegisterID* TypeOfResolveNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    Variable var = generator.variable(m_ident);
    if (RegisterID* local = var.local()) {
        generator.emitTDZCheckIfNecessary(var, local, nullptr);
        if (dst == generator.ignoredResult())
            return nullptr;
        return generator.emitTypeOf(generator.finalDestination(dst), local);
    }

    RefPtr<RegisterID> scope = generator.emitResolveScope(dst, var);
    RefPtr<RegisterID> value = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, DoNotThrowIfNotFound);
    generator.emitTDZCheckIfNecessary(var, value.get(), nullptr);
    if (dst == generator.ignoredResult())
        return nullptr;
    return generator.emitTypeOf(generator.finalDestination(dst, scope.get()), value.get());
}

// ------------------------------ TypeOfValueNode -----------------------------------

RegisterID* TypeOfValueNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult()) {
        generator.emitNodeInIgnoreResultPosition(m_expr);
        return nullptr;
    }
    RefPtr<RegisterID> src = generator.emitNode(m_expr);
    return generator.emitTypeOf(generator.finalDestination(dst), src.get());
}

// ------------------------------ PrefixNode ----------------------------------

RegisterID* PrefixNode::emitResolve(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(m_expr->isResolveNode());
    ResolveNode* resolve = static_cast<ResolveNode*>(m_expr);
    const Identifier& ident = resolve->identifier();

    Variable var = generator.variable(ident);
    if (RegisterID* local = var.local()) {
        generator.emitTDZCheckIfNecessary(var, local, nullptr);
        RefPtr<RegisterID> localReg = local;
        if (var.isReadOnly()) {
            generator.emitReadOnlyExceptionIfNeeded(var);
            localReg = generator.move(generator.tempDestination(dst), localReg.get());
        } else if (generator.shouldEmitTypeProfilerHooks()) {
            RefPtr<RegisterID> tempDst = generator.tempDestination(dst);
            generator.move(tempDst.get(), localReg.get());
            emitIncOrDec(generator, tempDst.get(), m_operator);
            generator.move(localReg.get(), tempDst.get());
            generator.emitProfileType(localReg.get(), var, divotStart(), divotEnd());
            return generator.move(dst, tempDst.get());
        }
        emitIncOrDec(generator, localReg.get(), m_operator);
        return generator.move(dst, localReg.get());
    }

    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RefPtr<RegisterID> scope = generator.emitResolveScope(dst, var);
    RefPtr<RegisterID> value = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
    generator.emitTDZCheckIfNecessary(var, value.get(), nullptr);
    if (var.isReadOnly()) {
        bool threwException = generator.emitReadOnlyExceptionIfNeeded(var);
        if (threwException)
            return value.get();
    }

    emitIncOrDec(generator, value.get(), m_operator);
    if (!var.isReadOnly()) {
        generator.emitPutToScope(scope.get(), var, value.get(), generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, InitializationMode::NotInitialization);
        generator.emitProfileType(value.get(), var, divotStart(), divotEnd());
    }
    return generator.move(dst, value.get());
}

RegisterID* PrefixNode::emitBracket(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(m_expr->isBracketAccessorNode());
    BracketAccessorNode* bracketAccessor = static_cast<BracketAccessorNode*>(m_expr);
    ExpressionNode* baseNode = bracketAccessor->base();
    ExpressionNode* subscript = bracketAccessor->subscript();

    RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(baseNode, bracketAccessor->subscriptHasAssignments(), subscript->isPure(generator));
    RefPtr<RegisterID> property = generator.emitNodeForProperty(subscript);
    if (!subscript->isNumber() && !subscript->isString()) {
        // Never double-evaluate the subscript expression;
        // don't even evaluate it once if the base isn't subscriptable.
        generator.emitRequireObjectCoercible(base.get(), "Cannot access property of undefined or null"_s);
        property = generator.emitToPropertyKeyOrNumber(generator.newTemporary(), property.get());
    }
    RefPtr<RegisterID> propDst = generator.tempDestination(dst);

    generator.emitExpressionInfo(bracketAccessor->divot(), bracketAccessor->divotStart(), bracketAccessor->divotEnd());
    RegisterID* value;
    RefPtr<RegisterID> thisValue;
    if (baseNode->isSuperNode()) {
        thisValue = generator.ensureThis();
        value = generator.emitGetByVal(propDst.get(), base.get(), thisValue.get(), property.get());
    } else
        value = generator.emitGetByVal(propDst.get(), base.get(), property.get());
    emitIncOrDec(generator, value, m_operator);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    if (baseNode->isSuperNode())
        generator.emitPutByVal(base.get(), thisValue.get(), property.get(), value);
    else
        generator.emitPutByVal(base.get(), property.get(), value);
    generator.emitProfileType(value, divotStart(), divotEnd());
    return generator.move(dst, propDst.get());
}

RegisterID* PrefixNode::emitDot(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(m_expr->isDotAccessorNode());
    DotAccessorNode* dotAccessor = static_cast<DotAccessorNode*>(m_expr);
    ExpressionNode* baseNode = dotAccessor->base();
    const Identifier& ident = dotAccessor->identifier();

    RefPtr<RegisterID> base = generator.emitNode(baseNode);
    RefPtr<RegisterID> propDst = generator.tempDestination(dst);

    generator.emitExpressionInfo(dotAccessor->divot(), dotAccessor->divotStart(), dotAccessor->divotEnd());
    RegisterID* value;
    if (dotAccessor->isPrivateMember()) {
        auto privateTraits = generator.getPrivateTraits(ident);
        if (privateTraits.isField()) {
            ASSERT(!baseNode->isSuperNode());
            Variable var = generator.variable(ident);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            RefPtr<RegisterID> privateName = generator.newTemporary();
            generator.emitGetFromScope(privateName.get(), scope.get(), var, DoNotThrowIfNotFound);

            value = generator.emitGetPrivateName(propDst.get(), base.get(), privateName.get());
            emitIncOrDec(generator, value, m_operator);
            generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
            generator.emitPrivateFieldPut(base.get(), privateName.get(), value);
            generator.emitProfileType(value, divotStart(), divotEnd());
            return generator.move(dst, propDst.get());
        }

        if (privateTraits.isMethod()) {
            Variable var = generator.variable(ident);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            ASSERT(scope); // Private names are always captured.
            RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
            generator.emitCheckPrivateBrand(base.get(), privateBrandSymbol.get(), privateTraits.isStatic());

            generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
            generator.emitThrowTypeError("Trying to access an undefined private setter"_s);
            return generator.move(dst, propDst.get());
        }

        Variable var = generator.variable(ident);
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        ASSERT(scope); // Private names are always captured.
        RefPtr<RegisterID> privateBrandSymbol = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
        generator.emitCheckPrivateBrand(base.get(), privateBrandSymbol.get(), privateTraits.isStatic());

        if (privateTraits.isGetter()) {
            RefPtr<RegisterID> getterSetterObj = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
            RefPtr<RegisterID> getterFunction = generator.emitDirectGetById(generator.newTemporary(), getterSetterObj.get(), generator.propertyNames().builtinNames().getPrivateName());
            CallArguments args(generator, nullptr);
            generator.move(args.thisRegister(), base.get());
            value = generator.emitCall(propDst.get(), getterFunction.get(), NoExpectedFunction, args, m_position, m_position, m_position, DebuggableCall::Yes);
        } else {
            generator.emitThrowTypeError("Trying to access an undefined private getter"_s);
            return generator.move(dst, propDst.get());
        }

        emitIncOrDec(generator, value, m_operator);
        generator.emitExpressionInfo(divot(), divotStart(), divotEnd());

        if (privateTraits.isSetter()) {
            RefPtr<RegisterID> getterSetterObj = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
            RefPtr<RegisterID> setterFunction = generator.emitDirectGetById(generator.newTemporary(), getterSetterObj.get(), generator.propertyNames().builtinNames().setPrivateName());
            CallArguments args(generator, nullptr, 1);
            generator.move(args.thisRegister(), base.get());
            generator.move(args.argumentRegister(0), value);
            generator.emitCallIgnoreResult(generator.newTemporary(), setterFunction.get(), NoExpectedFunction, args, m_position, m_position, m_position, DebuggableCall::Yes);
            generator.emitProfileType(value, divotStart(), divotEnd());
            return generator.move(dst, propDst.get());
        } 

        generator.emitThrowTypeError("Trying to access an undefined private getter"_s);
        return generator.move(dst, propDst.get());
    }

    RefPtr<RegisterID> thisValue;
    if (baseNode->isSuperNode()) {
        thisValue = generator.ensureThis();
        value = generator.emitGetById(propDst.get(), base.get(), thisValue.get(), ident);
    } else
        value = generator.emitGetById(propDst.get(), base.get(), ident);
    emitIncOrDec(generator, value, m_operator);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    if (baseNode->isSuperNode())
        generator.emitPutById(base.get(), thisValue.get(), ident, value);
    else
        generator.emitPutById(base.get(), ident, value);
    generator.emitProfileType(value, divotStart(), divotEnd());
    return generator.move(dst, propDst.get());
}

RegisterID* PrefixNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (m_expr->isResolveNode())
        return emitResolve(generator, dst);

    if (m_expr->isBracketAccessorNode())
        return emitBracket(generator, dst);

    if (m_expr->isDotAccessorNode())
        return emitDot(generator, dst);

    ASSERT(m_expr->isFunctionCall());
    generator.emitNode(m_expr);
    return emitThrowReferenceError(generator, m_operator == Operator::PlusPlus
        ? "Prefix ++ operator applied to value that is not a reference."_s
        : "Prefix -- operator applied to value that is not a reference."_s,
        dst);
}

// ------------------------------ Unary Operation Nodes -----------------------------------

RegisterID* UnaryOpNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult()) {
        // op_not is not user-observable. We can skip it completely if the result is not used.
        // This is used in the wild, for example,
        // ```
        //     !(function (a) {
        //          ...
        //     })(a);
        // ```
        if (opcodeID() == op_not) {
            generator.emitNodeInIgnoreResultPosition(m_expr);
            return nullptr;
        }
    }
    RefPtr<RegisterID> src = generator.emitNode(m_expr);
    generator.emitExpressionInfo(position(), position(), position());
    return generator.emitUnaryOp(opcodeID(), generator.finalDestination(dst), src.get(), m_expr->resultDescriptor());
}

// ------------------------------ UnaryPlusNode -----------------------------------

RegisterID* UnaryPlusNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(opcodeID() == op_to_number);
    RefPtr<RegisterID> src = generator.emitNode(expr());
    generator.emitExpressionInfo(position(), position(), position());
    return generator.emitToNumber(generator.finalDestination(dst), src.get());
}
 
// ------------------------------ LogicalNotNode -----------------------------------

void LogicalNotNode::emitBytecodeInConditionContext(BytecodeGenerator& generator, Label& trueTarget, Label& falseTarget, FallThroughMode fallThroughMode)
{
    if (needsDebugHook()) [[unlikely]]
        generator.emitDebugHook(this);

    // Reverse the true and false targets.
    generator.emitNodeInConditionContext(expr(), falseTarget, trueTarget, invert(fallThroughMode));
}


// ------------------------------ Binary Operation Nodes -----------------------------------

// BinaryOpNode::emitStrcat:
//
// This node generates an op_strcat operation.  This opcode can handle concatenation of three or
// more values, where we can determine a set of separate op_add operations would be operating on
// string values.
//
// This function expects to be operating on a graph of AST nodes looking something like this:
//
//     (a)...     (b)
//          \   /
//           (+)     (c)
//              \   /
//      [d]     ((+))
//         \    /
//          [+=]
//
// The assignment operation is optional, if it exists the register holding the value on the
// lefthand side of the assignment should be passing as the optional 'lhs' argument.
//
// The method should be called on the node at the root of the tree of regular binary add
// operations (marked in the diagram with a double set of parentheses).  This node must
// be performing a string concatenation (determined by statically detecting that at least
// one child must be a string).  
//
// Since the minimum number of values being concatenated together is expected to be 3, if
// a lhs to a concatenating assignment is not provided then the  root add should have at
// least one left child that is also an add that can be determined to be operating on strings.
//
RegisterID* BinaryOpNode::emitStrcat(BytecodeGenerator& generator, RegisterID* dst, RegisterID* lhs, ReadModifyResolveNode* emitExpressionInfoForMe)
{
    ASSERT(isAdd());
    ASSERT(resultDescriptor().definitelyIsString());

    // Create a list of expressions for all the adds in the tree of nodes we can convert into
    // a string concatenation.  The rightmost node (c) is added first.  The rightmost node is
    // added first, and the leftmost child is never added, so the vector produced for the
    // example above will be [ c, b ].
    Vector<ExpressionNode*, 16> reverseExpressionList;
    reverseExpressionList.append(m_expr2);

    // Examine the left child of the add.  So long as this is a string add, add its right-child
    // to the list, and keep processing along the left fork.
    ExpressionNode* leftMostAddChild = m_expr1;
    while (leftMostAddChild->isAdd() && leftMostAddChild->resultDescriptor().definitelyIsString()) {
        reverseExpressionList.append(static_cast<AddNode*>(leftMostAddChild)->m_expr2);
        leftMostAddChild = static_cast<AddNode*>(leftMostAddChild)->m_expr1;
    }

    Vector<RefPtr<RegisterID>, 16> temporaryRegisters;

    // If there is an assignment, allocate a temporary to hold the lhs after conversion.
    // We could possibly avoid this (the lhs is converted last anyway, we could let the
    // op_strcat node handle its conversion if required).
    if (lhs)
        temporaryRegisters.append(generator.newTemporary());

    // Emit code for the leftmost node ((a) in the example).
    temporaryRegisters.append(generator.newTemporary());
    RegisterID* leftMostAddChildTempRegister = temporaryRegisters.last().get();
    generator.emitNode(leftMostAddChildTempRegister, leftMostAddChild);

    // Note on ordering of conversions:
    //
    // We maintain the same ordering of conversions as we would see if the concatenations
    // was performed as a sequence of adds (otherwise this optimization could change
    // behaviour should an object have been provided a valueOf or toString method).
    //
    // Considering the above example, the sequnce of execution is:
    //     * evaluate operand (a)
    //     * evaluate operand (b)
    //     * convert (a) to primitive   <-  (this would be triggered by the first add)
    //     * convert (b) to primitive   <-  (ditto)
    //     * evaluate operand (c)
    //     * convert (c) to primitive   <-  (this would be triggered by the second add)
    // And optionally, if there is an assignment:
    //     * convert (d) to primitive   <-  (this would be triggered by the assigning addition)
    //
    // As such we do not plant an op to convert the leftmost child now.  Instead, use
    // 'leftMostAddChildTempRegister' as a flag to trigger generation of the conversion
    // once the second node has been generated.  However, if the leftmost child is an
    // immediate we can trivially determine that no conversion will be required.
    // If this is the case
    if (leftMostAddChild->isString())
        leftMostAddChildTempRegister = nullptr;

    while (reverseExpressionList.size()) {
        ExpressionNode* node = reverseExpressionList.last();
        reverseExpressionList.removeLast();

        // Emit the code for the current node.
        temporaryRegisters.append(generator.newTemporary());
        generator.emitNode(temporaryRegisters.last().get(), node);

        // On the first iteration of this loop, when we first reach this point we have just
        // generated the second node, which means it is time to convert the leftmost operand.
        if (leftMostAddChildTempRegister) {
            generator.emitToPrimitive(leftMostAddChildTempRegister, leftMostAddChildTempRegister);
            leftMostAddChildTempRegister = nullptr; // Only do this once.
        }
        // Plant a conversion for this node, if necessary.
        if (!node->isString())
            generator.emitToPrimitive(temporaryRegisters.last().get(), temporaryRegisters.last().get());
    }
    ASSERT(temporaryRegisters.size() >= 3);

    // Certain read-modify nodes require expression info to be emitted *after* m_right has been generated.
    // If this is required the node is passed as 'emitExpressionInfoForMe'; do so now.
    if (emitExpressionInfoForMe)
        generator.emitExpressionInfo(emitExpressionInfoForMe->divot(), emitExpressionInfoForMe->divotStart(), emitExpressionInfoForMe->divotEnd());
    // If there is an assignment convert the lhs now.  This will also copy lhs to
    // the temporary register we allocated for it.
    if (lhs)
        generator.emitToPrimitive(temporaryRegisters[0].get(), lhs);

    return generator.emitStrcat(generator.finalDestination(dst, temporaryRegisters[0].get()), temporaryRegisters[0].get(), temporaryRegisters.size());
}

void BinaryOpNode::emitBytecodeInConditionContext(BytecodeGenerator& generator, Label& trueTarget, Label& falseTarget, FallThroughMode fallThroughMode)
{
    TriState branchCondition;
    ExpressionNode* branchExpression;
    tryFoldToBranch(generator, branchCondition, branchExpression);

    if (needsDebugHook()) [[unlikely]] {
        if (branchCondition != TriState::Indeterminate)
            generator.emitDebugHook(this);
    }

    if (branchCondition == TriState::Indeterminate)
        ExpressionNode::emitBytecodeInConditionContext(generator, trueTarget, falseTarget, fallThroughMode);
    else if (branchCondition == TriState::True)
        generator.emitNodeInConditionContext(branchExpression, trueTarget, falseTarget, fallThroughMode);
    else
        generator.emitNodeInConditionContext(branchExpression, falseTarget, trueTarget, invert(fallThroughMode));
}

static inline bool canFoldToBranch(OpcodeID opcodeID, ExpressionNode* branchExpression, JSValue constant)
{
    ResultType expressionType = branchExpression->resultDescriptor();

    if (expressionType.definitelyIsBoolean() && constant.isBoolean())
        return true;
    else if (expressionType.definitelyIsBoolean() && constant.isInt32() && (constant.asInt32() == 0 || constant.asInt32() == 1))
        return opcodeID == op_eq || opcodeID == op_neq; // Strict equality is false in the case of type mismatch.
    else if (expressionType.isInt32() && constant.isInt32() && constant.asInt32() == 0)
        return true;

    return false;
}

void BinaryOpNode::tryFoldToBranch(BytecodeGenerator& generator, TriState& branchCondition, ExpressionNode*& branchExpression)
{
    branchCondition = TriState::Indeterminate;
    branchExpression = nullptr;

    ConstantNode* constant = nullptr;
    if (m_expr1->isConstant()) {
        constant = static_cast<ConstantNode*>(m_expr1);
        branchExpression = m_expr2;
    } else if (m_expr2->isConstant()) {
        constant = static_cast<ConstantNode*>(m_expr2);
        branchExpression = m_expr1;
    }

    if (!constant)
        return;
    ASSERT(branchExpression);

    OpcodeID opcodeID = this->opcodeID();
    JSValue value = constant->jsValue(generator);
    if (!value) [[unlikely]]
        return;
    bool canFoldToBranch = JSC::canFoldToBranch(opcodeID, branchExpression, value);
    if (!canFoldToBranch)
        return;

    if (opcodeID == op_eq || opcodeID == op_stricteq)
        branchCondition = triState(value.pureToBoolean() != TriState::False);
    else if (opcodeID == op_neq || opcodeID == op_nstricteq)
        branchCondition = triState(value.pureToBoolean() == TriState::False);
}

RegisterID* BinaryOpNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    OpcodeID opcodeID = this->opcodeID();

    if (opcodeID == op_less || opcodeID == op_lesseq || opcodeID == op_greater || opcodeID == op_greatereq) {
        auto isUInt32 = [&] (ExpressionNode* node) -> std::optional<UInt32Result> {
            if (node->isBinaryOpNode() && static_cast<BinaryOpNode*>(node)->opcodeID() == op_urshift)
                return UInt32Result::UInt32;
            if (node->isNumber() && static_cast<NumberNode*>(node)->isIntegerNode()) {
                auto value = jsNumber(static_cast<NumberNode*>(node)->value());
                if (value.isInt32() && value.asInt32() >= 0)
                    return UInt32Result::Constant;
            }
            return std::nullopt;
        };
        auto leftResult = isUInt32(m_expr1);
        auto rightResult = isUInt32(m_expr2);
        if ((leftResult && rightResult) && (leftResult.value() == UInt32Result::UInt32 || rightResult.value() == UInt32Result::UInt32)) {
            auto* left = m_expr1;
            auto* right = m_expr2;
            if (left->isBinaryOpNode()) {
                ASSERT(static_cast<BinaryOpNode*>(left)->opcodeID() == op_urshift);
                static_cast<BinaryOpNode*>(left)->m_shouldToUnsignedResult = false;
            }
            if (right->isBinaryOpNode()) {
                ASSERT(static_cast<BinaryOpNode*>(right)->opcodeID() == op_urshift);
                static_cast<BinaryOpNode*>(right)->m_shouldToUnsignedResult = false;
            }
            RefPtr<RegisterID> src1 = generator.emitNodeForLeftHandSide(left, m_rightHasAssignments, right->isPure(generator));
            RefPtr<RegisterID> src2 = generator.emitNode(right);
            generator.emitExpressionInfo(position(), position(), position());

            // Since the both sides only accept Int32, replacing operands is not observable to users.
            bool replaceOperands = false;
            OpcodeID resultOp = opcodeID;
            switch (opcodeID) {
            case op_less:
                resultOp = op_below;
                break;
            case op_lesseq:
                resultOp = op_beloweq;
                break;
            case op_greater:
                resultOp = op_below;
                replaceOperands = true;
                break;
            case op_greatereq:
                resultOp = op_beloweq;
                replaceOperands = true;
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            OperandTypes operandTypes(left->resultDescriptor(), right->resultDescriptor());
            if (replaceOperands) {
                std::swap(src1, src2);
                operandTypes = OperandTypes(right->resultDescriptor(), left->resultDescriptor());
            }
            return generator.emitBinaryOp(resultOp, generator.finalDestination(dst, src1.get()), src1.get(), src2.get(), operandTypes);
        }
    }

    if (opcodeID == op_add && m_expr1->isAdd() && m_expr1->resultDescriptor().definitelyIsString()) {
        generator.emitExpressionInfo(position(), position(), position());
        return emitStrcat(generator, dst);
    }

    if (opcodeID == op_neq) {
        if (m_expr1->isNull() || m_expr2->isNull()) {
            RefPtr<RegisterID> src = generator.tempDestination(dst);
            generator.emitNode(src.get(), m_expr1->isNull() ? m_expr2 : m_expr1);
            return generator.emitUnaryOp<OpNeqNull>(generator.finalDestination(dst, src.get()), src.get());
        }
    }

    ExpressionNode* left = m_expr1;
    ExpressionNode* right = m_expr2;
    if (opcodeID == op_neq || opcodeID == op_nstricteq) {
        if (left->isString())
            std::swap(left, right);
    }

    RefPtr<RegisterID> src1 = generator.emitNodeForLeftHandSide(left, m_rightHasAssignments, right->isPure(generator));
    bool wasTypeof = generator.lastOpcodeID() == op_typeof;
    RefPtr<RegisterID> src2 = generator.emitNode(right);
    generator.emitExpressionInfo(position(), position(), position());
    if (wasTypeof && (opcodeID == op_neq || opcodeID == op_nstricteq)) {
        RefPtr<RegisterID> tmp = generator.tempDestination(dst);
        if (opcodeID == op_neq)
            generator.emitEqualityOp<OpEq>(generator.finalDestination(tmp.get(), src1.get()), src1.get(), src2.get());
        else if (opcodeID == op_nstricteq)
            generator.emitEqualityOp<OpStricteq>(generator.finalDestination(tmp.get(), src1.get()), src1.get(), src2.get());
        else
            RELEASE_ASSERT_NOT_REACHED();
        return generator.emitUnaryOp<OpNot>(generator.finalDestination(dst, tmp.get()), tmp.get());
    }
    RegisterID* result = generator.emitBinaryOp(opcodeID, generator.finalDestination(dst, src1.get()), src1.get(), src2.get(), OperandTypes(left->resultDescriptor(), right->resultDescriptor()));
    if (m_shouldToUnsignedResult) {
        if (opcodeID == op_urshift && dst != generator.ignoredResult())
            return generator.emitUnaryOp<OpUnsigned>(result, result);
    }
    return result;
}

RegisterID* EqualNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (m_expr1->isNull() || m_expr2->isNull()) {
        RefPtr<RegisterID> src = generator.tempDestination(dst);
        generator.emitNode(src.get(), m_expr1->isNull() ? m_expr2 : m_expr1);
        return generator.emitUnaryOp<OpEqNull>(generator.finalDestination(dst, src.get()), src.get());
    }

    ExpressionNode* left = m_expr1;
    ExpressionNode* right = m_expr2;
    if (left->isString())
        std::swap(left, right);

    RefPtr<RegisterID> src1 = generator.emitNodeForLeftHandSide(left, m_rightHasAssignments, m_expr2->isPure(generator));
    RefPtr<RegisterID> src2 = generator.emitNode(right);
    return generator.emitEqualityOp<OpEq>(generator.finalDestination(dst, src1.get()), src1.get(), src2.get());
}

RegisterID* StrictEqualNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ExpressionNode* left = m_expr1;
    ExpressionNode* right = m_expr2;
    if (left->isString())
        std::swap(left, right);

    RefPtr<RegisterID> src1 = generator.emitNodeForLeftHandSide(left, m_rightHasAssignments, m_expr2->isPure(generator));
    RefPtr<RegisterID> src2 = generator.emitNode(right);
    return generator.emitEqualityOp<OpStricteq>(generator.finalDestination(dst, src1.get()), src1.get(), src2.get());
}

RegisterID* ThrowableBinaryOpNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> src1 = generator.emitNodeForLeftHandSide(m_expr1, m_rightHasAssignments, m_expr2->isPure(generator));
    RefPtr<RegisterID> src2 = generator.emitNode(m_expr2);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    return generator.emitBinaryOp(opcodeID(), generator.finalDestination(dst, src1.get()), src1.get(), src2.get(), OperandTypes(m_expr1->resultDescriptor(), m_expr2->resultDescriptor()));
}

RegisterID* InstanceOfNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> value = generator.emitNodeForLeftHandSide(m_expr1, m_rightHasAssignments, m_expr2->isPure(generator));
    RefPtr<RegisterID> dstReg = generator.finalDestination(dst, value.get());
    RefPtr<RegisterID> constructor = generator.emitNode(m_expr2);
    RefPtr<RegisterID> hasInstanceOrPrototype = generator.newTemporary();
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    return generator.emitInstanceof(dstReg.get(), value.get(), constructor.get(), hasInstanceOrPrototype.get());
}

// ------------------------------ InNode ----------------------------

RegisterID* InNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (m_expr1->isPrivateIdentifier()) {
        RefPtr<RegisterID> base = generator.emitNode(m_expr2);

        auto identifier = static_cast<PrivateIdentifierNode*>(m_expr1)->value();
        auto privateTraits = generator.getPrivateTraits(identifier);
        Variable var = generator.variable(identifier);
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        ASSERT(scope); // Private names are always captured.

        if (privateTraits.isField()) {
            RefPtr<RegisterID> privateName = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, DoNotThrowIfNotFound);
            return generator.emitHasPrivateName(generator.finalDestination(dst, base.get()), base.get(), privateName.get());
        }

        ASSERT(privateTraits.isPrivateMethodOrAccessor());
        RefPtr<RegisterID> privateBrand = generator.emitGetPrivateBrand(generator.newTemporary(), scope.get(), privateTraits.isStatic());
        return generator.emitHasPrivateBrand(generator.finalDestination(dst, base.get()), base.get(), privateBrand.get(), privateTraits.isStatic());
    }

    if (isNonIndexStringElement(*m_expr1)) {
        RefPtr<RegisterID> base = generator.emitNode(m_expr2);
        generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
        return generator.emitInById(generator.finalDestination(dst, base.get()), base.get(), static_cast<StringNode*>(m_expr1)->value());
    }

    RefPtr<RegisterID> key = generator.emitNodeForLeftHandSide(m_expr1, m_rightHasAssignments, m_expr2->isPure(generator));
    RefPtr<RegisterID> base = generator.emitNode(m_expr2);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    return generator.emitInByVal(generator.finalDestination(dst, key.get()), key.get(), base.get());
}


// ------------------------------ LogicalOpNode ----------------------------

RegisterID* LogicalOpNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult()) {
        Ref<Label> afterExpr1 = generator.newLabel();
        Ref<Label> afterExpr2 = generator.newLabel();
        if (m_operator == LogicalOperator::And)
            generator.emitNodeInConditionContext(m_expr1, afterExpr1.get(), afterExpr2.get(), FallThroughMeansTrue);
        else
            generator.emitNodeInConditionContext(m_expr1, afterExpr2.get(), afterExpr1.get(), FallThroughMeansFalse);
        generator.emitLabel(afterExpr1.get());

        generator.emitNodeInTailPosition(dst, m_expr2);
        generator.emitLabel(afterExpr2.get());
        return dst;
    }

    RefPtr<RegisterID> temp = generator.tempDestination(dst);
    Ref<Label> target = generator.newLabel();
    
    generator.emitNode(temp.get(), m_expr1);
    if (m_operator == LogicalOperator::And)
        generator.emitJumpIfFalse(temp.get(), target.get());
    else
        generator.emitJumpIfTrue(temp.get(), target.get());
    generator.emitNodeInTailPosition(temp.get(), m_expr2);
    generator.emitLabel(target.get());

    return generator.move(dst, temp.get());
}

void LogicalOpNode::emitBytecodeInConditionContext(BytecodeGenerator& generator, Label& trueTarget, Label& falseTarget, FallThroughMode fallThroughMode)
{
    if (needsDebugHook()) [[unlikely]]
        generator.emitDebugHook(this);

    Ref<Label> afterExpr1 = generator.newLabel();
    if (m_operator == LogicalOperator::And)
        generator.emitNodeInConditionContext(m_expr1, afterExpr1.get(), falseTarget, FallThroughMeansTrue);
    else 
        generator.emitNodeInConditionContext(m_expr1, trueTarget, afterExpr1.get(), FallThroughMeansFalse);
    generator.emitLabel(afterExpr1.get());

    generator.emitNodeInConditionContext(m_expr2, trueTarget, falseTarget, fallThroughMode);
}

// ------------------------------ CoalesceNode ----------------------------

RegisterID* CoalesceNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> temp = generator.tempDestination(dst);
    Ref<Label> endLabel = generator.newLabel();

    if (m_hasAbsorbedOptionalChain)
        generator.pushOptionalChainTarget();
    generator.emitNode(temp.get(), m_expr1);
    generator.emitJumpIfFalse(generator.emitIsUndefinedOrNull(generator.newTemporary(), temp.get()), endLabel.get());

    if (m_hasAbsorbedOptionalChain)
        generator.popOptionalChainTarget();
    generator.emitNodeInTailPosition(temp.get(), m_expr2);

    generator.emitLabel(endLabel.get());
    return generator.move(dst, temp.get());
}

// ------------------------------ OptionalChainNode ----------------------------

RegisterID* OptionalChainNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> finalDest = generator.finalDestination(dst);

    if (m_isOutermost)
        generator.pushOptionalChainTarget();
    generator.emitNodeInTailPosition(finalDest.get(), m_expr);
    if (m_isOutermost)
        generator.popOptionalChainTarget(finalDest.get(), m_expr->isDeleteNode());

    return finalDest.get();
}

// ------------------------------ ConditionalNode ------------------------------

RegisterID* ConditionalNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> newDst = generator.finalDestination(dst);
    Ref<Label> beforeElse = generator.newLabel();
    Ref<Label> afterElse = generator.newLabel();

    Ref<Label> beforeThen = generator.newLabel();
    generator.emitNodeInConditionContext(m_logical, beforeThen.get(), beforeElse.get(), FallThroughMeansTrue);
    generator.emitLabel(beforeThen.get());

    generator.emitProfileControlFlow(m_expr1->startOffset());
    generator.emitNodeInTailPosition(newDst.get(), m_expr1);
    generator.emitJump(afterElse.get());

    generator.emitLabel(beforeElse.get());
    generator.emitProfileControlFlow(m_expr1->endOffset() + 1);
    generator.emitNodeInTailPosition(newDst.get(), m_expr2);

    generator.emitLabel(afterElse.get());

    generator.emitProfileControlFlow(m_expr2->endOffset() + 1);

    return newDst.get();
}

// ------------------------------ ReadModifyResolveNode -----------------------------------

// FIXME: should this be moved to be a method on BytecodeGenerator?
static ALWAYS_INLINE RegisterID* emitReadModifyAssignment(BytecodeGenerator& generator, RegisterID* dst, RegisterID* src1, ExpressionNode* m_right, Operator oper, OperandTypes types, ReadModifyResolveNode* emitExpressionInfoForMe = nullptr, Variable* emitReadOnlyExceptionIfNeededForMe = nullptr)
{
    OpcodeID opcodeID;
    switch (oper) {
    case Operator::MultEq:
        opcodeID = op_mul;
        break;
    case Operator::DivEq:
        opcodeID = op_div;
        break;
    case Operator::PlusEq:
        if (m_right->isAdd() && m_right->resultDescriptor().definitelyIsString()) {
            RegisterID* result = static_cast<AddNode*>(m_right)->emitStrcat(generator, dst, src1, emitExpressionInfoForMe);
            if (emitReadOnlyExceptionIfNeededForMe)
                generator.emitReadOnlyExceptionIfNeeded(*emitReadOnlyExceptionIfNeededForMe);
            return result;
        }

        opcodeID = op_add;
        break;
    case Operator::MinusEq:
        opcodeID = op_sub;
        break;
    case Operator::LShift:
        opcodeID = op_lshift;
        break;
    case Operator::RShift:
        opcodeID = op_rshift;
        break;
    case Operator::URShift:
        opcodeID = op_urshift;
        break;
    case Operator::BitAndEq:
        opcodeID = op_bitand;
        break;
    case Operator::BitXOrEq:
        opcodeID = op_bitxor;
        break;
    case Operator::BitOrEq:
        opcodeID = op_bitor;
        break;
    case Operator::ModEq:
        opcodeID = op_mod;
        break;
    case Operator::PowEq:
        opcodeID = op_pow;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return dst;
    }

    RegisterID* src2 = generator.emitNode(m_right);

    if (emitReadOnlyExceptionIfNeededForMe) {
        bool threwException = generator.emitReadOnlyExceptionIfNeeded(*emitReadOnlyExceptionIfNeededForMe);
        if (threwException)
            return src2;
    }

    // Certain read-modify nodes require expression info to be emitted *after* m_right has been generated.
    // If this is required the node is passed as 'emitExpressionInfoForMe'; do so now.
    if (emitExpressionInfoForMe)
        generator.emitExpressionInfo(emitExpressionInfoForMe->divot(), emitExpressionInfoForMe->divotStart(), emitExpressionInfoForMe->divotEnd());

    RegisterID* result = generator.emitBinaryOp(opcodeID, dst, src1, src2, types);
    if (oper == Operator::URShift)
        return generator.emitUnaryOp<OpUnsigned>(result, result);
    return result;
}

RegisterID* ReadModifyResolveNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    JSTextPosition newDivot = divotStart() + m_ident.length();
    Variable var = generator.variable(m_ident);
    if (RegisterID* local = var.local()) {
        generator.emitTDZCheckIfNecessary(var, local, nullptr);
        if (var.isReadOnly()) {
            RegisterID* result = emitReadModifyAssignment(generator, generator.finalDestination(dst), local, m_right, m_operator, OperandTypes(ResultType::unknownType(), m_right->resultDescriptor()), nullptr, &var);
            generator.emitProfileType(result, divotStart(), divotEnd());
            return result;
        }
        
        if (generator.leftHandSideNeedsCopy(m_rightHasAssignments, m_right->isPure(generator))) {
            RefPtr<RegisterID> result = generator.newTemporary();
            generator.move(result.get(), local);
            emitReadModifyAssignment(generator, result.get(), result.get(), m_right, m_operator, OperandTypes(ResultType::unknownType(), m_right->resultDescriptor()));
            generator.move(local, result.get());
            generator.emitProfileType(local, divotStart(), divotEnd());
            return generator.move(dst, result.get());
        }
        
        RegisterID* result = emitReadModifyAssignment(generator, local, local, m_right, m_operator, OperandTypes(ResultType::unknownType(), m_right->resultDescriptor()));
        generator.emitProfileType(result, divotStart(), divotEnd());
        return generator.move(dst, result);
    }

    generator.emitExpressionInfo(newDivot, divotStart(), newDivot);
    RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
    RefPtr<RegisterID> value = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, ThrowIfNotFound);
    generator.emitTDZCheckIfNecessary(var, value.get(), nullptr);
    RefPtr<RegisterID> result = emitReadModifyAssignment(generator, generator.finalDestination(dst, value.get()), value.get(), m_right, m_operator, OperandTypes(ResultType::unknownType(), m_right->resultDescriptor()), this, var.isReadOnly() ? &var : nullptr);
    RegisterID* returnResult = result.get();
    if (!var.isReadOnly()) {
        returnResult = generator.emitPutToScope(scope.get(), var, result.get(), generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, InitializationMode::NotInitialization);
        generator.emitProfileType(result.get(), var, divotStart(), divotEnd());
    }
    return returnResult;
}

// ------------------------------ ShortCircuitReadModifyResolveNode -----------------------------------

static ALWAYS_INLINE void emitShortCircuitAssignment(BytecodeGenerator& generator, RegisterID* value, Operator oper, Label& afterAssignment)
{
    switch (oper) {
    case Operator::CoalesceEq:
        generator.emitJumpIfFalse(generator.emitIsUndefinedOrNull(generator.newTemporary(), value), afterAssignment);
        break;

    case Operator::OrEq:
        generator.emitJumpIfTrue(value, afterAssignment);
        break;

    case Operator::AndEq:
        generator.emitJumpIfFalse(value, afterAssignment);
        break;

    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
}

RegisterID* ShortCircuitReadModifyResolveNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    JSTextPosition newDivot = divotStart() + m_ident.length();

    Variable var = generator.variable(m_ident);
    bool isReadOnly = var.isReadOnly();

    if (RefPtr<RegisterID> local = var.local()) {
        generator.emitTDZCheckIfNecessary(var, local.get(), nullptr);

        if (isReadOnly) {
            RefPtr<RegisterID> result = local;

            Ref<Label> afterAssignment = generator.newLabel();
            emitShortCircuitAssignment(generator, result.get(), m_operator, afterAssignment.get());

            generator.emitNode(result.get(), m_right); // Execute side effects first.
            bool threwException = generator.emitReadOnlyExceptionIfNeeded(var);

            if (!threwException)
                generator.emitProfileType(result.get(), divotStart(), divotEnd());

            generator.emitLabel(afterAssignment.get());
            return generator.move(dst, result.get());
        }

        if (generator.leftHandSideNeedsCopy(m_rightHasAssignments, m_right->isPure(generator))) {
            RefPtr<RegisterID> result = generator.tempDestination(dst);
            generator.move(result.get(), local.get());

            Ref<Label> afterAssignment = generator.newLabel();
            emitShortCircuitAssignment(generator, result.get(), m_operator, afterAssignment.get());

            generator.emitNode(result.get(), m_right);
            generator.move(local.get(), result.get());
            generator.emitProfileType(result.get(), var, divotStart(), divotEnd());

            generator.emitLabel(afterAssignment.get());
            return generator.move(dst, result.get());
        }

        RefPtr<RegisterID> result = local;

        Ref<Label> afterAssignment = generator.newLabel();
        emitShortCircuitAssignment(generator, result.get(), m_operator, afterAssignment.get());

        generator.emitNode(result.get(), m_right);
        generator.emitProfileType(result.get(), var, divotStart(), divotEnd());

        generator.emitLabel(afterAssignment.get());
        return generator.move(dst, result.get());
    }

    generator.emitExpressionInfo(newDivot, divotStart(), newDivot);
    RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);

    RefPtr<RegisterID> uncheckedResult = generator.newTemporary();

    generator.emitGetFromScope(uncheckedResult.get(), scope.get(), var, ThrowIfNotFound);
    generator.emitTDZCheckIfNecessary(var, uncheckedResult.get(), nullptr);

    Ref<Label> afterAssignment = generator.newLabel();
    emitShortCircuitAssignment(generator, uncheckedResult.get(), m_operator, afterAssignment.get());

    generator.emitNode(uncheckedResult.get(), m_right); // Execute side effects first.

    bool threwException = isReadOnly ? generator.emitReadOnlyExceptionIfNeeded(var) : false;

    if (!threwException)
        generator.emitExpressionInfo(divot(), divotStart(), divotEnd());

    if (!isReadOnly) {
        generator.emitPutToScope(scope.get(), var, uncheckedResult.get(), generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, InitializationMode::NotInitialization);
        generator.emitProfileType(uncheckedResult.get(), var, divotStart(), divotEnd());
    }

    generator.emitLabel(afterAssignment.get());
    return generator.move(generator.finalDestination(dst, uncheckedResult.get()), uncheckedResult.get());
}

// ------------------------------ AssignResolveNode -----------------------------------

static InitializationMode initializationModeForAssignmentContext(AssignmentContext assignmentContext)
{
    switch (assignmentContext) {
    case AssignmentContext::DeclarationStatement:
        return InitializationMode::Initialization;
    case AssignmentContext::ConstDeclarationStatement:
        return InitializationMode::ConstInitialization;
    case AssignmentContext::AssignmentExpression:
        return InitializationMode::NotInitialization;
    }

    ASSERT_NOT_REACHED();
    return InitializationMode::NotInitialization;
}

RegisterID* AssignResolveNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    Variable var = generator.variable(m_ident);
    bool isReadOnly = var.isReadOnly() && m_assignmentContext != AssignmentContext::ConstDeclarationStatement;
    if (RegisterID* local = var.local()) {
        RegisterID* result = nullptr;

        if (isReadOnly) {
            result = generator.emitNode(dst, m_right); // Execute side effects first.

            if (m_assignmentContext == AssignmentContext::AssignmentExpression)
                generator.emitTDZCheckIfNecessary(var, local, nullptr);

            generator.emitReadOnlyExceptionIfNeeded(var);
            generator.emitProfileType(result, var, divotStart(), divotEnd());
        } else if ((m_assignmentContext == AssignmentContext::AssignmentExpression && generator.needsTDZCheck(var)) || var.isSpecial()) {
            RefPtr<RegisterID> tempDst = generator.tempDestination(dst);
            generator.emitNode(tempDst.get(), m_right); // Execute side effects first.

            if (m_assignmentContext == AssignmentContext::AssignmentExpression)
                generator.emitTDZCheckIfNecessary(var, local, nullptr);

            generator.move(local, tempDst.get());
            generator.emitProfileType(local, var, divotStart(), divotEnd());
            result = generator.move(dst, tempDst.get());
        } else {
            RegisterID* right = generator.emitNode(local, m_right);
            generator.emitProfileType(right, var, divotStart(), divotEnd());
            result = generator.move(dst, right);
        }

        if (m_assignmentContext == AssignmentContext::DeclarationStatement || m_assignmentContext == AssignmentContext::ConstDeclarationStatement)
            generator.liftTDZCheckIfPossible(var);
        return result;
    }

    if (generator.ecmaMode().isStrict())
        generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
    if (m_assignmentContext == AssignmentContext::AssignmentExpression)
        generator.emitTDZCheckIfNecessary(var, nullptr, scope.get());
    if (dst == generator.ignoredResult())
        dst = nullptr;
    RefPtr<RegisterID> result = generator.emitNode(dst, m_right); // Execute side effects first.
    if (isReadOnly) {
        bool threwException = generator.emitReadOnlyExceptionIfNeeded(var);
        if (threwException)
            return result.get();
    }
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RegisterID* returnResult = result.get();
    if (!isReadOnly) {
        returnResult = generator.emitPutToScope(scope.get(), var, result.get(), generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, initializationModeForAssignmentContext(m_assignmentContext));
        generator.emitProfileType(result.get(), var, divotStart(), divotEnd());
    }

    if (m_assignmentContext == AssignmentContext::DeclarationStatement || m_assignmentContext == AssignmentContext::ConstDeclarationStatement)
        generator.liftTDZCheckIfPossible(var);
    return returnResult;
}

// ------------------------------ AssignDotNode -----------------------------------

RegisterID* AssignDotNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(m_base, m_rightHasAssignments, m_right->isPure(generator));
    RefPtr<RegisterID> value = generator.destinationForAssignResult(dst);
    RefPtr<RegisterID> result = generator.emitNode(value.get(), m_right);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RefPtr<RegisterID> forwardResult = (dst == generator.ignoredResult()) ? result.get() : generator.move(generator.tempDestination(result.get()), result.get());
    emitPutProperty(generator, base.get(), forwardResult.get());
    generator.emitProfileType(forwardResult.get(), divotStart(), divotEnd());
    return generator.move(dst, forwardResult.get());
}

// ------------------------------ ReadModifyDotNode -----------------------------------

RegisterID* ReadModifyDotNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(m_base, m_rightHasAssignments, m_right->isPure(generator));

    generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());
    RefPtr<RegisterID> thisValue;
    RefPtr<RegisterID> value = emitGetPropertyValue(generator, generator.tempDestination(dst), base.get(), thisValue);

    RegisterID* updatedValue = emitReadModifyAssignment(generator, generator.finalDestination(dst, value.get()), value.get(), m_right, m_operator, OperandTypes(ResultType::unknownType(), m_right->resultDescriptor()));

    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RefPtr<RegisterID> ret = emitPutProperty(generator, base.get(), updatedValue, thisValue);
    generator.emitProfileType(updatedValue, divotStart(), divotEnd());
    return ret.get();
}

// ------------------------------ ShortCircuitReadModifyDotNode -----------------------------------

RegisterID* ShortCircuitReadModifyDotNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(m_base, m_rightHasAssignments, m_right->isPure(generator));
    RefPtr<RegisterID> thisValue;

    RefPtr<RegisterID> result = generator.tempDestination(dst);

    generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());
    emitGetPropertyValue(generator, result.get(), base.get(), thisValue);
    Ref<Label> afterAssignment = generator.newLabel();
    emitShortCircuitAssignment(generator, result.get(), m_operator, afterAssignment.get());

    generator.emitNode(result.get(), m_right);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    emitPutProperty(generator, base.get(), result.get(), thisValue);
    generator.emitProfileType(result.get(), divotStart(), divotEnd());

    generator.emitLabel(afterAssignment.get());
    return generator.move(dst, result.get());
}

// ------------------------------ AssignErrorNode -----------------------------------

RegisterID* AssignErrorNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(m_left->isFunctionCall());
    generator.emitNode(m_left);
    return emitThrowReferenceError(generator, "Left side of assignment is not a reference."_s, dst);
}

// ------------------------------ AssignBracketNode -----------------------------------

RegisterID* AssignBracketNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ForInContext* context = nullptr;
    if (m_subscript->isResolveNode()) {
        Variable argumentVariable = generator.variable(static_cast<ResolveNode*>(m_subscript)->identifier());
        if (argumentVariable.isLocal()) {
            RegisterID* property = argumentVariable.local();
            context = generator.findForInContext(property);
        }
    }

    RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(m_base, m_subscriptHasAssignments || m_rightHasAssignments, m_subscript->isPure(generator) && m_right->isPure(generator));
    RefPtr<RegisterID> property = generator.emitNodeForLeftHandSideForProperty(m_subscript, m_rightHasAssignments, m_right->isPure(generator));
    RefPtr<RegisterID> value = generator.destinationForAssignResult(dst);
    RefPtr<RegisterID> result = generator.emitNode(value.get(), m_right);

    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    RegisterID* forwardResult = (dst == generator.ignoredResult()) ? result.get() : generator.move(generator.tempDestination(result.get()), result.get());

    if (isNonIndexStringElement(*m_subscript)) {
        if (m_base->isSuperNode()) {
            RefPtr<RegisterID> thisValue = generator.ensureThis();
            generator.emitPutById(base.get(), thisValue.get(), static_cast<StringNode*>(m_subscript)->value(), forwardResult);
        } else
            generator.emitPutById(base.get(), static_cast<StringNode*>(m_subscript)->value(), forwardResult);
    } else {
        if (m_base->isSuperNode()) {
            RefPtr<RegisterID> thisValue = generator.ensureThis();
            generator.emitPutByVal(base.get(), thisValue.get(), property.get(), forwardResult);
        } else {
            if (context)
                generator.emitEnumeratorPutByVal(*context, base.get(), property.get(), forwardResult);
            else
                generator.emitPutByVal(base.get(), property.get(), forwardResult);
        }
    }

    generator.emitProfileType(forwardResult, divotStart(), divotEnd());
    return generator.move(dst, forwardResult);
}

// ------------------------------ ReadModifyBracketNode -----------------------------------

RegisterID* ReadModifyBracketNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(m_base, m_subscriptHasAssignments || m_rightHasAssignments, m_subscript->isPure(generator) && m_right->isPure(generator));
    RefPtr<RegisterID> property = generator.emitNodeForLeftHandSideForProperty(m_subscript, m_rightHasAssignments, m_right->isPure(generator));
    if (!m_subscript->isNumber() && !m_subscript->isString()) {
        // Never double-evaluate the subscript expression;
        // don't even evaluate it once if the base isn't subscriptable.
        generator.emitRequireObjectCoercible(base.get(), "Cannot access property of undefined or null"_s);
        property = generator.emitToPropertyKeyOrNumber(generator.newTemporary(), property.get());
    }

    generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());
    RefPtr<RegisterID> value;
    RefPtr<RegisterID> thisValue;
    if (m_base->isSuperNode()) {
        thisValue = generator.ensureThis();
        value = generator.emitGetByVal(generator.tempDestination(dst), base.get(), thisValue.get(), property.get());
    } else
        value = generator.emitGetByVal(generator.tempDestination(dst), base.get(), property.get());
    RegisterID* updatedValue = emitReadModifyAssignment(generator, generator.finalDestination(dst, value.get()), value.get(), m_right, m_operator, OperandTypes(ResultType::unknownType(), m_right->resultDescriptor()));

    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    if (m_base->isSuperNode())
        generator.emitPutByVal(base.get(), thisValue.get(), property.get(), updatedValue);
    else
        generator.emitPutByVal(base.get(), property.get(), updatedValue);
    generator.emitProfileType(updatedValue, divotStart(), divotEnd());

    return updatedValue;
}

// ------------------------------ ShortCircuitReadModifyBracketNode -----------------------------------

RegisterID* ShortCircuitReadModifyBracketNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(m_base, m_subscriptHasAssignments || m_rightHasAssignments, m_subscript->isPure(generator) && m_right->isPure(generator));
    RefPtr<RegisterID> property = generator.emitNodeForLeftHandSideForProperty(m_subscript, m_rightHasAssignments, m_right->isPure(generator));
    if (!m_subscript->isNumber() && !m_subscript->isString()) {
        // Never double-evaluate the subscript expression;
        // don't even evaluate it once if the base isn't subscriptable.
        generator.emitRequireObjectCoercible(base.get(), "Cannot access property of undefined or null"_s);
        property = generator.emitToPropertyKeyOrNumber(generator.newTemporary(), property.get());
    }

    RefPtr<RegisterID> thisValue;
    RefPtr<RegisterID> result = generator.tempDestination(dst);

    generator.emitExpressionInfo(subexpressionDivot(), subexpressionStart(), subexpressionEnd());
    if (m_base->isSuperNode()) {
        thisValue = generator.ensureThis();
        generator.emitGetByVal(result.get(), base.get(), thisValue.get(), property.get());
    } else
        generator.emitGetByVal(result.get(), base.get(), property.get());

    Ref<Label> afterAssignment = generator.newLabel();
    emitShortCircuitAssignment(generator, result.get(), m_operator, afterAssignment.get());

    generator.emitNode(result.get(), m_right);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    if (m_base->isSuperNode())
        generator.emitPutByVal(base.get(), thisValue.get(), property.get(), result.get());
    else
        generator.emitPutByVal(base.get(), property.get(), result.get());
    generator.emitProfileType(result.get(), divotStart(), divotEnd());

    generator.emitLabel(afterAssignment.get());
    return generator.move(dst, result.get());
}

// ------------------------------ CommaNode ------------------------------------

RegisterID* CommaNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    CommaNode* node = this;
    for (; node->next(); node = node->next())
        generator.emitNodeInIgnoreResultPosition(node->m_expr);
    return generator.emitNodeInTailPosition(dst, node->m_expr);
}

// ------------------------------ SourceElements -------------------------------

inline void SourceElements::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    StatementNode* lastStatementWithCompletionValue = nullptr;
    if (generator.shouldBeConcernedWithCompletionValue()) {
        for (StatementNode* statement = m_head; statement; statement = statement->next()) {
            if (statement->hasCompletionValue())
                lastStatementWithCompletionValue = statement;
        }
    }

    for (StatementNode* statement = m_head; statement; statement = statement->next()) {
        if (generator.shouldBeConcernedWithCompletionValue()) {
            if (statement == lastStatementWithCompletionValue)
                generator.emitLoad(dst, jsUndefined());
            generator.emitNodeInTailPosition(dst, statement);
        } else
            generator.emitNodeInTailPosition(generator.ignoredResult(), statement);
    }
}

// ------------------------------ BlockNode ------------------------------------

void BlockNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (!m_statements)
        return;
    generator.pushLexicalScope(this, BytecodeGenerator::ScopeType::LetConstScope, BytecodeGenerator::TDZCheckOptimization::Optimize, BytecodeGenerator::NestedScopeType::IsNested);
    m_statements->emitBytecode(generator, dst);
    generator.popLexicalScope(this);
}

// ------------------------------ EmptyStatementNode ---------------------------

void EmptyStatementNode::emitBytecode(BytecodeGenerator&, RegisterID*)
{
}

// ------------------------------ DebuggerStatementNode ---------------------------

void DebuggerStatementNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    generator.emitDebugHook(DidReachDebuggerStatement, position());
}

// ------------------------------ ExprStatementNode ----------------------------

void ExprStatementNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(m_expr);
    generator.emitNodeInTailPositionFromExprStatementNode(dst, m_expr);
}

// ------------------------------ DeclarationStatement ----------------------------

void DeclarationStatement::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    ASSERT(m_expr);
    generator.emitNode(m_expr);
}

// ------------------------------ EmptyVarExpression ----------------------------

RegisterID* EmptyVarExpression::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    // It's safe to return null here because this node will always be a child node of DeclarationStatement which ignores our return value.
    if (!generator.shouldEmitTypeProfilerHooks())
        return nullptr;

    Variable var = generator.variable(m_ident);
    if (RegisterID* local = var.local())
        generator.emitProfileType(local, var, position(), position() + m_ident.length());
    else {
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        RefPtr<RegisterID> value = generator.emitGetFromScope(generator.newTemporary(), scope.get(), var, DoNotThrowIfNotFound);
        generator.emitProfileType(value.get(), var, position(), position() + m_ident.length());
    }

    return nullptr;
}

// ------------------------------ EmptyLetExpression ----------------------------

RegisterID* EmptyLetExpression::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    // Lexical declarations like 'let' must move undefined into their variables so we don't
    // get TDZ errors for situations like this: `let x; x;`
    Variable var = generator.variable(m_ident);
    if (RegisterID* local = var.local()) {
        generator.emitLoad(local, jsUndefined());
        generator.emitProfileType(local, var, position(), position() + m_ident.length());
    } else {
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        RefPtr<RegisterID> value = generator.emitLoad(nullptr, jsUndefined());
        generator.emitPutToScope(scope.get(), var, value.get(), generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, InitializationMode::Initialization);
        generator.emitProfileType(value.get(), var, position(), position() + m_ident.length()); 
    }

    generator.liftTDZCheckIfPossible(var);

    // It's safe to return null here because this node will always be a child node of DeclarationStatement which ignores our return value.
    return nullptr;
}

// ------------------------------ IfElseNode ---------------------------------------

static inline StatementNode* singleStatement(StatementNode* statementNode)
{
    if (statementNode->isBlock())
        return static_cast<BlockNode*>(statementNode)->singleStatement();
    return statementNode;
}

bool IfElseNode::tryFoldBreakAndContinue(BytecodeGenerator& generator, StatementNode* ifBlock,
    Label*& trueTarget, FallThroughMode& fallThroughMode)
{
    StatementNode* singleStatement = JSC::singleStatement(ifBlock);
    if (!singleStatement)
        return false;

    if (singleStatement->isBreak()) {
        BreakNode* breakNode = static_cast<BreakNode*>(singleStatement);
        Label* target = breakNode->trivialTarget(generator);
        if (!target)
            return false;
        trueTarget = target;
        fallThroughMode = FallThroughMeansFalse;
        return true;
    }

    if (singleStatement->isContinue()) {
        ContinueNode* continueNode = static_cast<ContinueNode*>(singleStatement);
        Label* target = continueNode->trivialTarget(generator);
        if (!target)
            return false;
        trueTarget = target;
        fallThroughMode = FallThroughMeansFalse;
        return true;
    }

    return false;
}

void IfElseNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (generator.shouldBeConcernedWithCompletionValue()) {
        if (m_ifBlock->hasEarlyBreakOrContinue() || (m_elseBlock && m_elseBlock->hasEarlyBreakOrContinue()))
            generator.emitLoad(dst, jsUndefined());
    }

    Ref<Label> beforeThen = generator.newLabel();
    Ref<Label> beforeElse = generator.newLabel();
    Ref<Label> afterElse = generator.newLabel();

    Label* trueTarget = beforeThen.ptr();
    Label& falseTarget = beforeElse.get();
    FallThroughMode fallThroughMode = FallThroughMeansTrue;
    bool didFoldIfBlock = tryFoldBreakAndContinue(generator, m_ifBlock, trueTarget, fallThroughMode);

    generator.emitNodeInConditionContext(m_condition, *trueTarget, falseTarget, fallThroughMode);
    generator.emitLabel(beforeThen.get());
    generator.emitProfileControlFlow(m_ifBlock->startOffset());

    if (!didFoldIfBlock) {
        generator.emitNodeInTailPosition(dst, m_ifBlock);
        if (m_elseBlock)
            generator.emitJump(afterElse.get());
    }

    generator.emitLabel(beforeElse.get());

    if (m_elseBlock) {
        generator.emitProfileControlFlow(m_ifBlock->endOffset() + (m_ifBlock->isBlock() ? 1 : 0));
        generator.emitNodeInTailPosition(dst, m_elseBlock);
    }

    generator.emitLabel(afterElse.get());
    StatementNode* endingBlock = m_elseBlock ? m_elseBlock : m_ifBlock;
    generator.emitProfileControlFlow(endingBlock->endOffset() + (endingBlock->isBlock() ? 1 : 0));
}

// ------------------------------ DoWhileNode ----------------------------------

void DoWhileNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (generator.shouldBeConcernedWithCompletionValue() && m_statement->hasEarlyBreakOrContinue())
        generator.emitLoad(dst, jsUndefined());

    Ref<LabelScope> scope = generator.newLabelScope(LabelScope::Loop);

    Ref<Label> topOfLoop = generator.newLabel();
    generator.emitLabel(topOfLoop.get());
    generator.emitLoopHint();

    generator.emitNodeInTailPosition(dst, m_statement);

    generator.emitLabel(*scope->continueTarget());
    generator.emitNodeInConditionContext(m_expr, topOfLoop.get(), scope->breakTarget(), FallThroughMeansFalse);

    generator.emitLabel(scope->breakTarget());
}

// ------------------------------ WhileNode ------------------------------------

void WhileNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (generator.shouldBeConcernedWithCompletionValue() && m_statement->hasEarlyBreakOrContinue())
        generator.emitLoad(dst, jsUndefined());

    Ref<LabelScope> scope = generator.newLabelScope(LabelScope::Loop);
    Ref<Label> topOfLoop = generator.newLabel();

    generator.emitNodeInConditionContext(m_expr, topOfLoop.get(), scope->breakTarget(), FallThroughMeansTrue);

    generator.emitLabel(topOfLoop.get());
    generator.emitLoopHint();
    
    generator.emitProfileControlFlow(m_statement->startOffset());
    generator.emitNodeInTailPosition(dst, m_statement);

    generator.emitLabel(*scope->continueTarget());

    generator.emitNodeInConditionContext(m_expr, topOfLoop.get(), scope->breakTarget(), FallThroughMeansFalse);

    generator.emitLabel(scope->breakTarget());

    generator.emitProfileControlFlow(m_statement->endOffset() + (m_statement->isBlock() ? 1 : 0));
}

// ------------------------------ ForNode --------------------------------------

void ForNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (generator.shouldBeConcernedWithCompletionValue() && m_statement->hasEarlyBreakOrContinue())
        generator.emitLoad(dst, jsUndefined());

    Ref<LabelScope> scope = generator.newLabelScope(LabelScope::Loop);

    RegisterID* forLoopSymbolTable = nullptr;
    generator.pushLexicalScope(this, BytecodeGenerator::ScopeType::LetConstScope, BytecodeGenerator::TDZCheckOptimization::Optimize, BytecodeGenerator::NestedScopeType::IsNested, &forLoopSymbolTable);

    if (m_expr1) {
        generator.emitNodeInIgnoreResultPosition(m_expr1);
        if (m_initializerContainsClosure)
            generator.prepareLexicalScopeForNextForLoopIteration(this, forLoopSymbolTable);
    }

    Ref<Label> topOfLoop = generator.newLabel();
    if (m_expr2)
        generator.emitNodeInConditionContext(m_expr2, topOfLoop.get(), scope->breakTarget(), FallThroughMeansTrue);

    generator.emitLabel(topOfLoop.get());
    generator.emitLoopHint();
    generator.emitProfileControlFlow(m_statement->startOffset());

    generator.emitNodeInTailPosition(dst, m_statement);

    generator.emitLabel(*scope->continueTarget());
    generator.prepareLexicalScopeForNextForLoopIteration(this, forLoopSymbolTable);
    if (m_expr3)
        generator.emitNodeInIgnoreResultPosition(m_expr3);

    if (m_expr2)
        generator.emitNodeInConditionContext(m_expr2, topOfLoop.get(), scope->breakTarget(), FallThroughMeansFalse);
    else
        generator.emitJump(topOfLoop.get());

    generator.emitLabel(scope->breakTarget());
    generator.popLexicalScope(this);
    generator.emitProfileControlFlow(m_statement->endOffset() + (m_statement->isBlock() ? 1 : 0));
}

// ------------------------------ ForInNode ------------------------------------

RegisterID* ForInNode::tryGetBoundLocal(BytecodeGenerator& generator)
{
    if (m_lexpr->isResolveNode()) {
        const Identifier& ident = static_cast<ResolveNode*>(m_lexpr)->identifier();
        return generator.variable(ident).local();
    }

    if (m_lexpr->isDestructuringNode()) {
        DestructuringAssignmentNode* assignNode = static_cast<DestructuringAssignmentNode*>(m_lexpr);
        auto binding = assignNode->bindings();
        if (!binding->isBindingNode())
            return nullptr;

        auto simpleBinding = static_cast<BindingNode*>(binding);
        const Identifier& ident = simpleBinding->boundProperty();
        Variable var = generator.variable(ident);
        if (var.isSpecial())
            return nullptr;
        return var.local();
    }

    return nullptr;
}

void ForInNode::emitLoopHeader(BytecodeGenerator& generator, RegisterID* propertyName)
{
    auto lambdaEmitResolveVariable = [&] (const Identifier& ident) {
        Variable var = generator.variable(ident);
        if (RegisterID* local = var.local()) {
            if (var.isReadOnly())
                generator.emitReadOnlyExceptionIfNeeded(var);
            generator.move(local, propertyName);
        } else {
            if (generator.ecmaMode().isStrict())
                generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
            if (var.isReadOnly())
                generator.emitReadOnlyExceptionIfNeeded(var);
            RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
            generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
            generator.emitPutToScope(scope.get(), var, propertyName, generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, InitializationMode::NotInitialization);
        }
        generator.emitProfileType(propertyName, var, m_lexpr->position(), m_lexpr->position() + ident.length());
    };

    if (m_lexpr->isResolveNode()) {
        const Identifier& ident = static_cast<ResolveNode*>(m_lexpr)->identifier();
        lambdaEmitResolveVariable(ident);
        return;
    }

    if (m_lexpr->isAssignResolveNode()) {
        const Identifier& ident = static_cast<AssignResolveNode*>(m_lexpr)->identifier();
        lambdaEmitResolveVariable(ident);
        return;
    }

    if (m_lexpr->isDotAccessorNode()) {
        DotAccessorNode* assignNode = static_cast<DotAccessorNode*>(m_lexpr);
        RefPtr<RegisterID> base = generator.emitNode(assignNode->base());
        generator.emitExpressionInfo(assignNode->divot(), assignNode->divotStart(), assignNode->divotEnd());
        assignNode->emitPutProperty(generator, base.get(), propertyName);
        generator.emitProfileType(propertyName, assignNode->divotStart(), assignNode->divotEnd());
        return;
    }

    if (m_lexpr->isBracketAccessorNode()) {
        BracketAccessorNode* assignNode = static_cast<BracketAccessorNode*>(m_lexpr);
        RefPtr<RegisterID> base = generator.emitNode(assignNode->base());
        RefPtr<RegisterID> subscript = generator.emitNodeForProperty(assignNode->subscript());
        generator.emitExpressionInfo(assignNode->divot(), assignNode->divotStart(), assignNode->divotEnd());
        if (assignNode->base()->isSuperNode()) {
            RefPtr<RegisterID> thisValue = generator.ensureThis();
            generator.emitPutByVal(base.get(), thisValue.get(), subscript.get(), propertyName);
        } else
            generator.emitPutByVal(base.get(), subscript.get(), propertyName);
        generator.emitProfileType(propertyName, assignNode->divotStart(), assignNode->divotEnd());
        return;
    }

    if (m_lexpr->isDestructuringNode()) {
        DestructuringAssignmentNode* assignNode = static_cast<DestructuringAssignmentNode*>(m_lexpr);
        auto binding = assignNode->bindings();
        if (!binding->isBindingNode()) {
            assignNode->bindings()->bindValue(generator, propertyName);
            return;
        }

        auto simpleBinding = static_cast<BindingNode*>(binding);
        const Identifier& ident = simpleBinding->boundProperty();
        Variable var = generator.variable(ident);
        if (!var.local() || var.isSpecial()) {
            assignNode->bindings()->bindValue(generator, propertyName);
            return;
        }
        generator.move(var.local(), propertyName);
        generator.emitProfileType(propertyName, var, simpleBinding->divotStart(), simpleBinding->divotEnd());
        return;
    }

    RELEASE_ASSERT_NOT_REACHED();
}

void ForInNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (!m_lexpr->isAssignResolveNode() && !m_lexpr->isAssignmentLocation()) {
        ASSERT(m_lexpr->isFunctionCall());
        generator.emitNode(m_lexpr);
        emitThrowReferenceError(generator, "Left side of for-in statement is not a reference."_s);
        return;
    }

    if (generator.shouldBeConcernedWithCompletionValue() && m_statement->hasEarlyBreakOrContinue())
        generator.emitLoad(dst, jsUndefined());

    RegisterID* forLoopSymbolTable = nullptr;
    generator.pushLexicalScope(this, BytecodeGenerator::ScopeType::LetConstScope, BytecodeGenerator::TDZCheckOptimization::Optimize, BytecodeGenerator::NestedScopeType::IsNested, &forLoopSymbolTable);

    if (m_lexpr->isAssignResolveNode())
        generator.emitNodeInIgnoreResultPosition(m_lexpr);

    RefPtr<RegisterID> base = generator.newTemporary();

    generator.emitNode(base.get(), m_expr);
    RefPtr<RegisterID> local = this->tryGetBoundLocal(generator);

    std::optional<Variable> baseVariable = generator.tryResolveVariable(m_expr);

    int profilerStartOffset = m_statement->startOffset();
    int profilerEndOffset = m_statement->endOffset() + (m_statement->isBlock() ? 1 : 0);

    {
        RefPtr<RegisterID> enumerator = generator.newTemporary();
        RefPtr<RegisterID> mode = generator.emitLoad(generator.newTemporary(), jsNumber(static_cast<unsigned>(JSPropertyNameEnumerator::InitMode)));
        RefPtr<RegisterID> index = generator.emitLoad(generator.newTemporary(), jsNumber(0));
        RefPtr<RegisterID> propertyName = generator.newTemporary();
        Ref<LabelScope> scope = generator.newLabelScope(LabelScope::Loop);

        enumerator = generator.emitGetPropertyEnumerator(generator.newTemporary(), base.get());
        generator.emitJumpIfEmptyPropertyNameEnumerator(enumerator.get(), scope->breakTarget());

        generator.emitLabel(*scope->continueTarget());
        generator.emitLoopHint();
        generator.prepareLexicalScopeForNextForLoopIteration(this, forLoopSymbolTable);
        generator.emitDebugHook(m_lexpr); // Pause at the assignment expression for each for..in iteration.

        // FIXME: We should have a way to see if anyone is actually using the propertyName for something other than a get_by_val. If not, we could eliminate the toString in this opcode.
        generator.emitEnumeratorNext(propertyName.get(), mode.get(), index.get(), base.get(), enumerator.get());
        generator.emitJumpIfSentinelString(propertyName.get(), scope->breakTarget());

        this->emitLoopHeader(generator, propertyName.get());

        generator.emitProfileControlFlow(profilerStartOffset);

        generator.pushForInScope(local.get(), propertyName.get(), index.get(), enumerator.get(), mode.get(), baseVariable);
        generator.emitNode(dst, m_statement);
        generator.popForInScope(local.get());

        generator.emitProfileControlFlow(profilerEndOffset);
        generator.emitJump(*scope->continueTarget());

        generator.emitLabel(scope->breakTarget());
    }

    generator.popLexicalScope(this);
    generator.emitProfileControlFlow(profilerEndOffset);
}

// ------------------------------ ForOfNode ------------------------------------
void ForOfNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (!m_lexpr->isAssignmentLocation()) {
        ASSERT(m_lexpr->isFunctionCall());
        generator.emitNode(m_lexpr);
        emitThrowReferenceError(generator, "Left side of for-of statement is not a reference."_s);
        return;
    }

    if (generator.shouldBeConcernedWithCompletionValue() && m_statement->hasEarlyBreakOrContinue())
        generator.emitLoad(dst, jsUndefined());

    RegisterID* forLoopSymbolTable = nullptr;
    generator.pushLexicalScope(this, BytecodeGenerator::ScopeType::LetConstScope, BytecodeGenerator::TDZCheckOptimization::Optimize, BytecodeGenerator::NestedScopeType::IsNested, &forLoopSymbolTable);
    auto extractor = scopedLambda<void(BytecodeGenerator&, RegisterID*)>([this, dst](BytecodeGenerator& generator, RegisterID* value)
    {
        if (m_lexpr->isResolveNode()) {
            const Identifier& ident = static_cast<ResolveNode*>(m_lexpr)->identifier();
            Variable var = generator.variable(ident);
            if (RegisterID* local = var.local()) {
                if (var.isReadOnly())
                    generator.emitReadOnlyExceptionIfNeeded(var);
                generator.move(local, value);
            } else {
                if (generator.ecmaMode().isStrict())
                    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
                if (var.isReadOnly())
                    generator.emitReadOnlyExceptionIfNeeded(var);
                RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
                generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
                generator.emitPutToScope(scope.get(), var, value, generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, InitializationMode::NotInitialization);
            }
            generator.emitProfileType(value, var, m_lexpr->position(), m_lexpr->position() + ident.length());
        } else if (m_lexpr->isDotAccessorNode()) {
            DotAccessorNode* assignNode = static_cast<DotAccessorNode*>(m_lexpr);
            RefPtr<RegisterID> base = generator.emitNode(assignNode->base());
            generator.emitExpressionInfo(assignNode->divot(), assignNode->divotStart(), assignNode->divotEnd());
            assignNode->emitPutProperty(generator, base.get(), value);
            generator.emitProfileType(value, assignNode->divotStart(), assignNode->divotEnd());
        } else if (m_lexpr->isBracketAccessorNode()) {
            BracketAccessorNode* assignNode = static_cast<BracketAccessorNode*>(m_lexpr);
            RefPtr<RegisterID> base = generator.emitNode(assignNode->base());
            RegisterID* subscript = generator.emitNodeForProperty(assignNode->subscript());
            
            generator.emitExpressionInfo(assignNode->divot(), assignNode->divotStart(), assignNode->divotEnd());
            if (assignNode->base()->isSuperNode()) {
                RefPtr<RegisterID> thisValue = generator.ensureThis();
                generator.emitPutByVal(base.get(), thisValue.get(), subscript, value);
            } else
                generator.emitPutByVal(base.get(), subscript, value);
            generator.emitProfileType(value, assignNode->divotStart(), assignNode->divotEnd());
        } else {
            ASSERT(m_lexpr->isDestructuringNode());
            DestructuringAssignmentNode* assignNode = static_cast<DestructuringAssignmentNode*>(m_lexpr);
            assignNode->bindings()->bindValue(generator, value);
        }
        generator.emitProfileControlFlow(m_statement->startOffset());
        generator.emitNode(dst, m_statement);
    });
    generator.emitEnumeration(this, m_expr, extractor, this, forLoopSymbolTable);
    generator.popLexicalScope(this);
    generator.emitProfileControlFlow(m_statement->endOffset() + (m_statement->isBlock() ? 1 : 0));
}

// ------------------------------ ContinueNode ---------------------------------

Label* ContinueNode::trivialTarget(BytecodeGenerator& generator)
{
    if (generator.shouldEmitDebugHooks())
        return nullptr;

    LabelScope* scope = generator.continueTarget(m_ident);
    ASSERT(scope);

    if (generator.labelScopeDepth() != scope->scopeDepth())
        return nullptr;

    return scope->continueTarget();
}

void ContinueNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    LabelScope* scope = generator.continueTarget(m_ident);
    ASSERT(scope);

    bool hasFinally = generator.emitJumpViaFinallyIfNeeded(scope->scopeDepth(), *scope->continueTarget());
    if (!hasFinally) {
        int lexicalScopeIndex = generator.labelScopeDepthToLexicalScopeIndex(scope->scopeDepth());
        generator.restoreScopeRegister(lexicalScopeIndex);
        generator.emitJump(*scope->continueTarget());
    }

    generator.emitProfileControlFlow(endOffset());
}

// ------------------------------ BreakNode ------------------------------------

Label* BreakNode::trivialTarget(BytecodeGenerator& generator)
{
    if (generator.shouldEmitDebugHooks())
        return nullptr;

    LabelScope* scope = generator.breakTarget(m_ident);
    ASSERT(scope);

    if (generator.labelScopeDepth() != scope->scopeDepth())
        return nullptr;

    return &scope->breakTarget();
}

void BreakNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    LabelScope* scope = generator.breakTarget(m_ident);
    ASSERT(scope);

    bool hasFinally = generator.emitJumpViaFinallyIfNeeded(scope->scopeDepth(), scope->breakTarget());
    if (!hasFinally) {
        int lexicalScopeIndex = generator.labelScopeDepthToLexicalScopeIndex(scope->scopeDepth());
        generator.restoreScopeRegister(lexicalScopeIndex);
        generator.emitJump(scope->breakTarget());
    }

    generator.emitProfileControlFlow(endOffset());
}

// ------------------------------ ReturnNode -----------------------------------

void ReturnNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(generator.codeType() == FunctionCode);

    if (dst == generator.ignoredResult())
        dst = nullptr;

    RefPtr<RegisterID> returnRegister = nullptr;
    if (m_value) {
        returnRegister = generator.emitNodeInTailPositionFromReturnNode(dst, m_value);
        if (generator.parseMode() == SourceParseMode::AsyncGeneratorBodyMode)
            returnRegister = generator.emitAwait(generator.newTemporary(), returnRegister.get(), position());
    } else
        returnRegister = generator.emitLoad(dst, jsUndefined());

    generator.emitProfileType(returnRegister.get(), ProfileTypeBytecodeFunctionReturnStatement, divotStart(), divotEnd());

    bool hasFinally = generator.emitReturnViaFinallyIfNeeded(returnRegister.get());
    if (!hasFinally) {
        generator.emitWillLeaveCallFrameDebugHook();
        generator.emitReturn(returnRegister.get());
    }

    generator.emitProfileControlFlow(endOffset());
    // Emitting an unreachable return here is needed in case this op_profile_control_flow is the 
    // last opcode in a CodeBlock because a CodeBlock's instructions must end with a terminal opcode.
    if (generator.shouldEmitControlFlowProfilerHooks())
        generator.emitReturn(generator.emitLoad(nullptr, jsUndefined()));
}

// ------------------------------ WithNode -------------------------------------

void WithNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> scope = generator.emitNode(m_expr);
    generator.emitExpressionInfo(m_divot, m_divot - m_expressionLength, m_divot);
    generator.emitPushWithScope(scope.get());
    if (generator.shouldBeConcernedWithCompletionValue() && m_statement->hasEarlyBreakOrContinue())
        generator.emitLoad(dst, jsUndefined());
    generator.emitNodeInTailPosition(dst, m_statement);
    generator.emitPopWithScope();
}

// ------------------------------ CaseClauseNode --------------------------------

inline void CaseClauseNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    generator.emitProfileControlFlow(m_startOffset);
    if (!m_statements)
        return;
    m_statements->emitBytecode(generator, dst);
}

// ------------------------------ CaseBlockNode --------------------------------

enum SwitchKind { 
    SwitchUnset = 0,
    SwitchNumber = 1,
    SwitchString = 2,
    SwitchNeither = 3
};

static void processClauseList(ClauseListNode* list, Vector<ExpressionNode*, 8>& literalVector, SwitchKind& typeForTable, bool& singleCharacterSwitch, int32_t& min_num, int32_t& max_num)
{
    for (; list; list = list->getNext()) {
        ExpressionNode* clauseExpression = list->getClause()->expr();
        literalVector.append(clauseExpression);
        if (clauseExpression->isNumber()) {
            double value = static_cast<NumberNode*>(clauseExpression)->value();
            int32_t intVal = static_cast<int32_t>(value);
            if ((typeForTable & ~SwitchNumber) || (intVal != value)) {
                typeForTable = SwitchNeither;
                break;
            }
            if (intVal < min_num)
                min_num = intVal;
            if (intVal > max_num)
                max_num = intVal;
            typeForTable = SwitchNumber;
            continue;
        }
        if (clauseExpression->isString()) {
            if (typeForTable & ~SwitchString) {
                typeForTable = SwitchNeither;
                break;
            }
            auto& value = static_cast<StringNode*>(clauseExpression)->value().string();
            if (singleCharacterSwitch &= value.length() == 1) {
                int32_t intVal = value[0];
                if (intVal < min_num)
                    min_num = intVal;
                if (intVal > max_num)
                    max_num = intVal;
            }
            typeForTable = SwitchString;
            continue;
        }
        typeForTable = SwitchNeither;
        break;        
    }
}

SwitchInfo::SwitchType CaseBlockNode::tryTableSwitch(Vector<ExpressionNode*, 8>& literalVector, int32_t& min_num, int32_t& max_num)
{
    SwitchKind typeForTable = SwitchUnset;
    bool singleCharacterSwitch = true;
    
    processClauseList(m_list1, literalVector, typeForTable, singleCharacterSwitch, min_num, max_num);
    processClauseList(m_list2, literalVector, typeForTable, singleCharacterSwitch, min_num, max_num);

    if (literalVector.size() < s_tableSwitchMinimum)
        return SwitchInfo::SwitchType::None;
    
    if (typeForTable == SwitchUnset || typeForTable == SwitchNeither)
        return SwitchInfo::SwitchType::None;
    
    if (typeForTable == SwitchNumber) {
        int32_t range = max_num - min_num;
        if (min_num <= max_num) {
            if (range <= 1000 && (range / literalVector.size()) < Options::switchJumpTableAmountThreshold())
                return SwitchInfo::SwitchType::Immediate;
            return SwitchInfo::SwitchType::ImmediateList;
        }
        return SwitchInfo::SwitchType::None;
    } 
    
    ASSERT(typeForTable == SwitchString);
    
    if (singleCharacterSwitch) {
        int32_t range = max_num - min_num;
        if (min_num <= max_num) {
            if (range <= 1000 && (range / literalVector.size()) < Options::switchJumpTableAmountThreshold())
                return SwitchInfo::SwitchType::Character;
            return SwitchInfo::SwitchType::CharacterList;
        }
    }

    return SwitchInfo::SwitchType::String;
}

void CaseBlockNode::emitBytecodeForBlock(BytecodeGenerator& generator, RegisterID* switchExpression, RegisterID* dst)
{
    Vector<Ref<Label>, 8> labelVector;
    Vector<ExpressionNode*, 8> literalVector;
    int32_t min_num = std::numeric_limits<int32_t>::max();
    int32_t max_num = std::numeric_limits<int32_t>::min();
    SwitchInfo::SwitchType switchType = tryTableSwitch(literalVector, min_num, max_num);

    Ref<Label> defaultLabel = generator.newLabel();
    if (switchType != SwitchInfo::SwitchType::None) {
        // Prepare the various labels
        for (uint32_t i = 0; i < literalVector.size(); i++)
            labelVector.append(generator.newLabel());
        generator.beginSwitch(switchExpression, switchType);
    } else {
        // Setup jumps
        for (ClauseListNode* list = m_list1; list; list = list->getNext()) {
            RefPtr<RegisterID> clauseVal = generator.emitNode(list->getClause()->expr());
            Ref<Label> clauseLabel = generator.newLabel();
            labelVector.append(clauseLabel);
            generator.emitJumpIfTrue(generator.emitEqualityOp<OpStricteq>(generator.newTemporary(), clauseVal.get(), switchExpression), clauseLabel.get());
        }
        
        for (ClauseListNode* list = m_list2; list; list = list->getNext()) {
            RefPtr<RegisterID> clauseVal = generator.emitNode(list->getClause()->expr());
            Ref<Label> clauseLabel = generator.newLabel();
            labelVector.append(clauseLabel);
            generator.emitJumpIfTrue(generator.emitEqualityOp<OpStricteq>(generator.newTemporary(), clauseVal.get(), switchExpression), clauseLabel.get());
        }
        generator.emitJump(defaultLabel.get());
    }

    size_t i = 0;
    for (ClauseListNode* list = m_list1; list; list = list->getNext()) {
        generator.emitLabel(labelVector[i++].get());
        list->getClause()->emitBytecode(generator, dst);
    }

    if (m_defaultClause) {
        generator.emitLabel(defaultLabel.get());
        m_defaultClause->emitBytecode(generator, dst);
    }

    for (ClauseListNode* list = m_list2; list; list = list->getNext()) {
        generator.emitLabel(labelVector[i++].get());
        list->getClause()->emitBytecode(generator, dst);
    }
    if (!m_defaultClause)
        generator.emitLabel(defaultLabel.get());

    ASSERT(i == labelVector.size());
    if (switchType != SwitchInfo::SwitchType::None) {
        ASSERT(labelVector.size() == literalVector.size());
        generator.endSwitch(labelVector, literalVector.mutableSpan().data(), defaultLabel.get(), min_num, max_num);
    }
}

// ------------------------------ SwitchNode -----------------------------------

void SwitchNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (generator.shouldBeConcernedWithCompletionValue())
        generator.emitLoad(dst, jsUndefined());

    Ref<LabelScope> scope = generator.newLabelScope(LabelScope::Switch);

    RefPtr<RegisterID> r0 = generator.emitNode(m_expr);

    generator.pushLexicalScope(this, BytecodeGenerator::ScopeType::LetConstScope, BytecodeGenerator::TDZCheckOptimization::DoNotOptimize, BytecodeGenerator::NestedScopeType::IsNested);
    m_block->emitBytecodeForBlock(generator, r0.get(), dst);
    generator.popLexicalScope(this);

    generator.emitLabel(scope->breakTarget());
    generator.emitProfileControlFlow(endOffset());
}

// ------------------------------ LabelNode ------------------------------------

void LabelNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(!generator.breakTarget(m_name));

    Ref<LabelScope> scope = generator.newLabelScope(LabelScope::NamedLabel, &m_name);
    generator.emitNodeInTailPosition(dst, m_statement);

    generator.emitLabel(scope->breakTarget());
}

// ------------------------------ ThrowNode ------------------------------------

void ThrowNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (dst == generator.ignoredResult())
        dst = nullptr;
    RefPtr<RegisterID> expr = generator.emitNode(m_expr);
    generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
    generator.emitThrow(expr.get());

    generator.emitProfileControlFlow(endOffset()); 
}

// ------------------------------ TryNode --------------------------------------

void TryNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    // NOTE: The catch and finally blocks must be labeled explicitly, so the
    // optimizer knows they may be jumped to from anywhere.
    ASSERT(m_catchBlock || m_finallyBlock);

    RefPtr<RegisterID> tryCatchDst = dst;
    if (generator.shouldBeConcernedWithCompletionValue()) {
        if (m_finallyBlock)
            tryCatchDst = generator.newTemporary();

        if (m_finallyBlock || m_tryBlock->hasEarlyBreakOrContinue())
            generator.emitLoad(tryCatchDst.get(), jsUndefined());
    }

    RefPtr<Label> catchLabel;
    RefPtr<Label> catchEndLabel;
    RefPtr<Label> finallyLabel;
    RefPtr<Label> finallyEndLabel;
    std::optional<FinallyContext> finallyContext;

    if (m_finallyBlock) {
        finallyLabel = generator.newLabel();
        finallyEndLabel = generator.newLabel();

        finallyContext.emplace(generator, *finallyLabel);
        generator.pushFinallyControlFlowScope(finallyContext.value());
    }
    if (m_catchBlock) {
        catchLabel = generator.newLabel();
        catchEndLabel = generator.newLabel();
    }

    Ref<Label> tryLabel = generator.newEmittedLabel();
    Label& tryHandlerLabel = m_catchBlock ? *catchLabel : *finallyLabel;
    HandlerType tryHandlerType = m_catchBlock ? HandlerType::Catch : HandlerType::Finally;
    TryData* tryData = generator.pushTry(tryLabel.get(), tryHandlerLabel, tryHandlerType);
    TryData* finallyTryData = nullptr;
    if (!m_catchBlock && m_finallyBlock)
        finallyTryData = tryData;

    unsigned localScopeCountBeforeTryBlock = generator.localScopeCount();
    auto scopeRegisterMayBeClobbered = [&]() {
        return generator.localScopeCount() > localScopeCountBeforeTryBlock;
    };

    if (tryCatchDst == generator.ignoredResult())
        generator.emitNodeInIgnoreResultPosition(m_tryBlock);
    else
        generator.emitNode(tryCatchDst.get(), m_tryBlock);

    if (m_catchBlock) {
        if (m_finallyBlock)
            generator.emitJump(*finallyLabel);
        else
            generator.emitJump(*catchEndLabel);
    }

    Ref<Label> tryEndLabel = generator.newEmittedLabel();
    generator.popTry(tryData, tryEndLabel.get());

    if (m_catchBlock) {
        // Uncaught exception path: the catch block.
        generator.emitLabel(*catchLabel);
        RefPtr<RegisterID> thrownValueRegister = generator.newTemporary();
        RegisterID* completionTypeRegister = m_finallyBlock ? finallyContext->completionTypeRegister() : nullptr;
        generator.emitOutOfLineCatchHandler(thrownValueRegister.get(), completionTypeRegister, tryData);
        if (scopeRegisterMayBeClobbered())
            generator.restoreScopeRegister();

        if (m_finallyBlock) {
            // If the catch block throws an exception and we have a finally block, then the finally
            // block should "catch" that exception.
            finallyTryData = generator.pushTry(*catchLabel, *finallyLabel, HandlerType::Finally);
        }

        if (m_catchPattern) {
            generator.emitPushCatchScope(m_lexicalVariables, m_catchPattern->isBindingNode() ? BytecodeGenerator::ScopeType::CatchScopeWithSimpleParameter : BytecodeGenerator::ScopeType::CatchScope);
            m_catchPattern->bindValue(generator, thrownValueRegister.get());
        }

        generator.emitProfileControlFlow(m_tryBlock->endOffset() + 1);

        if (generator.shouldBeConcernedWithCompletionValue())
            generator.emitLoad(tryCatchDst.get(), jsUndefined());

        if (m_finallyBlock) {
            if (tryCatchDst == generator.ignoredResult())
                generator.emitNodeInIgnoreResultPosition(m_catchBlock);
            else
                generator.emitNode(tryCatchDst.get(), m_catchBlock);
        } else
            generator.emitNodeInTailPosition(tryCatchDst.get(), m_catchBlock);

        if (m_catchPattern)
            generator.emitPopCatchScope(m_lexicalVariables);

        if (m_finallyBlock) {
            generator.emitLoad(finallyContext->completionTypeRegister(), CompletionType::Normal);
            generator.popTry(finallyTryData, *finallyLabel);
        }

        generator.emitLabel(*catchEndLabel);
        generator.emitProfileControlFlow(m_catchBlock->endOffset() + 1);
    }

    if (m_finallyBlock) {
        generator.popFinallyControlFlowScope();

        // Entry to the finally block for CompletionType::Throw to be generated later.
        generator.emitOutOfLineFinallyHandler(finallyContext->completionValueRegister(), finallyContext->completionTypeRegister(), finallyTryData);

        // Entry to the finally block for CompletionTypes other than Throw.
        generator.emitLabel(*finallyLabel);
        if (scopeRegisterMayBeClobbered())
            generator.restoreScopeRegister();

        int finallyStartOffset = m_catchBlock ? m_catchBlock->endOffset() + 1 : m_tryBlock->endOffset() + 1;
        
        // The completion value of a finally block is ignored *just* when it is a normal completion.
        if (generator.shouldBeConcernedWithCompletionValue()) {
            ASSERT(dst != tryCatchDst.get());
            if (m_finallyBlock->hasEarlyBreakOrContinue())
                generator.emitLoad(dst, jsUndefined());

            generator.emitProfileControlFlow(finallyStartOffset);
            generator.emitNodeInTailPosition(dst, m_finallyBlock);

            generator.move(dst, tryCatchDst.get());
        } else {
            generator.emitProfileControlFlow(finallyStartOffset);
            generator.emitNodeInTailPosition(m_finallyBlock);
        }

        generator.emitFinallyCompletion(finallyContext.value(), *finallyEndLabel);
        generator.emitLabel(*finallyEndLabel);
        generator.emitProfileControlFlow(m_finallyBlock->endOffset() + 1);
    }
}

// ------------------------------ ScopeNode -----------------------------

inline void ScopeNode::emitStatementsBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (!m_statements)
        return;
    m_statements->emitBytecode(generator, dst);
}

static void emitProgramNodeBytecode(BytecodeGenerator& generator, ScopeNode& scopeNode)
{
    generator.emitDebugHook(WillExecuteProgram, JSTextPosition(scopeNode.startLine(), scopeNode.startStartOffset(), scopeNode.startLineStartOffset()));

    RefPtr<RegisterID> dstRegister = generator.newTemporary();
    generator.emitLoad(dstRegister.get(), jsUndefined());
    generator.emitProfileControlFlow(scopeNode.startStartOffset());
    scopeNode.emitStatementsBytecode(generator, dstRegister.get());

    generator.emitDebugHook(DidExecuteProgram, JSTextPosition(scopeNode.lastLine(), scopeNode.startOffset(), scopeNode.lineStartOffset()));
    generator.emitEnd(dstRegister.get());
}

// ------------------------------ ProgramNode -----------------------------

void ProgramNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    emitProgramNodeBytecode(generator, *this);
}

// ------------------------------ ModuleProgramNode --------------------

void ModuleProgramNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    emitProgramNodeBytecode(generator, *this);
}

// ------------------------------ EvalNode -----------------------------

void EvalNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    generator.emitDebugHook(WillExecuteProgram, JSTextPosition(startLine(), startStartOffset(), startLineStartOffset()));

    RefPtr<RegisterID> dstRegister = generator.newTemporary();
    generator.emitLoad(dstRegister.get(), jsUndefined());
    emitStatementsBytecode(generator, dstRegister.get());

    generator.emitDebugHook(DidExecuteProgram, JSTextPosition(lastLine(), startOffset(), lineStartOffset()));
    generator.emitEnd(dstRegister.get());
}

// ------------------------------ FunctionNode -----------------------------

void FunctionNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    if (generator.shouldEmitTypeProfilerHooks()) {
        // If the parameter list is non simple one, it is handled in bindValue's code.
        if (m_parameters->isSimpleParameterList()) {
            for (size_t i = 0; i < m_parameters->size(); i++) {
                BindingNode* bindingNode = static_cast<BindingNode*>(m_parameters->at(i).first);
                RegisterID reg(CallFrame::argumentOffset(i));
                generator.emitProfileType(&reg, ProfileTypeBytecodeFunctionArgument, bindingNode->divotStart(), bindingNode->divotEnd());
            }
        }
    }

    generator.emitProfileControlFlow(startStartOffset());
    generator.emitDebugHook(DidEnterCallFrame, JSTextPosition(startLine(), startStartOffset(), startLineStartOffset()));

    switch (generator.parseMode()) {
    case SourceParseMode::GeneratorWrapperFunctionMode:
    case SourceParseMode::GeneratorWrapperMethodMode:
    case SourceParseMode::AsyncGeneratorWrapperMethodMode:
    case SourceParseMode::AsyncGeneratorWrapperFunctionMode: {
        StatementNode* singleStatement = this->singleStatement();
        ASSERT(singleStatement->isExprStatement());
        ExprStatementNode* exprStatement = static_cast<ExprStatementNode*>(singleStatement);
        ExpressionNode* expr = exprStatement->expr();
        ASSERT(expr->isFuncExprNode());
        FuncExprNode* funcExpr = static_cast<FuncExprNode*>(expr);

        RefPtr<RegisterID> next = generator.newTemporary();
        generator.emitNode(next.get(), funcExpr);

        if (generator.superBinding() == SuperBinding::Needed) {
            RefPtr<RegisterID> homeObject = emitHomeObjectForCallee(generator);
            emitPutHomeObject(generator, next.get(), homeObject.get());
        }

        if (isGeneratorWrapperParseMode(generator.parseMode()))
            generator.emitPutGeneratorFields(next.get());
        else {
            ASSERT(isAsyncGeneratorWrapperParseMode(generator.parseMode()));
            generator.emitPutAsyncGeneratorFields(next.get());
        }
        
        ASSERT(startOffset() >= lineStartOffset());
        generator.emitDebugHook(WillLeaveCallFrame, JSTextPosition(lastLine(), startOffset(), lineStartOffset()));
        generator.emitReturn(generator.generatorRegister());
        break;
    }

    case SourceParseMode::AsyncFunctionMode:
    case SourceParseMode::AsyncMethodMode:
    case SourceParseMode::AsyncArrowFunctionMode: {
        StatementNode* singleStatement = this->singleStatement();
        ASSERT(singleStatement->isExprStatement());
        ExprStatementNode* exprStatement = static_cast<ExprStatementNode*>(singleStatement);
        ExpressionNode* expr = exprStatement->expr();
        ASSERT(expr->isFuncExprNode());
        FuncExprNode* funcExpr = static_cast<FuncExprNode*>(expr);

        RefPtr<RegisterID> next = generator.newTemporary();
        generator.emitNode(next.get(), funcExpr);

        if (generator.superBinding() == SuperBinding::Needed || (generator.parseMode() == SourceParseMode::AsyncArrowFunctionMode && generator.isSuperUsedInInnerArrowFunction())) {
            RefPtr<RegisterID> homeObject = emitHomeObjectForCallee(generator);
            emitPutHomeObject(generator, next.get(), homeObject.get());
        }
        
        if (generator.parseMode() == SourceParseMode::AsyncArrowFunctionMode && generator.isThisUsedInInnerArrowFunction())
            generator.emitLoadThisFromArrowFunctionLexicalEnvironment();

        generator.emitPutGeneratorFields(next.get());

        ASSERT(startOffset() >= lineStartOffset());
        generator.emitDebugHook(WillLeaveCallFrame, JSTextPosition(lastLine(), startOffset(), lineStartOffset()));

        // load and call @asyncFunctionResume
        RefPtr<RegisterID> asyncFunctionResume = generator.moveLinkTimeConstant(nullptr, LinkTimeConstant::asyncFunctionResume);

        CallArguments args(generator, nullptr, 3);
        unsigned argumentCount = 0;
        generator.emitLoad(args.thisRegister(), jsUndefined());
        generator.move(args.argumentRegister(argumentCount++), generator.generatorRegister());
        generator.emitLoad(args.argumentRegister(argumentCount++), jsUndefined());
        generator.emitLoad(args.argumentRegister(argumentCount++), JSGenerator::ResumeMode::NormalMode);
        // JSTextPosition(int _line, int _offset, int _lineStartOffset)
        JSTextPosition divot(firstLine(), startOffset(), lineStartOffset());

        generator.emitCallIgnoreResult(generator.newTemporary(), asyncFunctionResume.get(), NoExpectedFunction, args, divot, divot, divot, DebuggableCall::No);
        generator.emitReturn(generator.promiseRegister());
        break;
    }

    case SourceParseMode::AsyncGeneratorBodyMode:
    case SourceParseMode::GeneratorBodyMode: {
        Ref<Label> generatorBodyLabel = generator.newLabel();
        {
            generator.emitJumpIfTrue(generator.emitEqualityOp<OpStricteq>(generator.newTemporary(), generator.generatorResumeModeRegister(), generator.emitLoad(nullptr, JSGenerator::ResumeMode::NormalMode)), generatorBodyLabel.get());

            Ref<Label> throwLabel = generator.newLabel();
            generator.emitJumpIfTrue(generator.emitEqualityOp<OpStricteq>(generator.newTemporary(), generator.generatorResumeModeRegister(), generator.emitLoad(nullptr, JSGenerator::ResumeMode::ThrowMode)), throwLabel.get());

            generator.emitReturn(generator.generatorValueRegister());

            generator.emitLabel(throwLabel.get());
            generator.emitThrow(generator.generatorValueRegister());
        }

        generator.emitLabel(generatorBodyLabel.get());
        [[fallthrough]];
    }

    case SourceParseMode::AsyncArrowFunctionBodyMode:
    case SourceParseMode::AsyncFunctionBodyMode: {
        emitStatementsBytecode(generator, generator.ignoredResult());

        generator.emitReturn(generator.emitLoad(nullptr, jsUndefined()));
        break;
    }

    default: {
        emitStatementsBytecode(generator, generator.ignoredResult());

        StatementNode* singleStatement = this->singleStatement();
        ReturnNode* returnNode = nullptr;

        // Check for a return statement at the end of a function composed of a single block.
        if (singleStatement && singleStatement->isBlock()) {
            StatementNode* lastStatementInBlock = static_cast<BlockNode*>(singleStatement)->lastStatement();
            if (lastStatementInBlock && lastStatementInBlock->isReturnNode())
                returnNode = static_cast<ReturnNode*>(lastStatementInBlock);
        }

        // If there is no return we must automatically insert one.
        if (!returnNode) {
            RegisterID* r0 = nullptr;
            if (generator.isConstructor() && generator.constructorKind() != ConstructorKind::Naked)
                r0 = generator.ensureThis();
            else
                r0 = generator.emitLoad(nullptr, jsUndefined());
            generator.emitProfileType(r0, ProfileTypeBytecodeFunctionReturnStatement); // Do not emit expression info for this profile because it's not in the user's source code.
            ASSERT(startOffset() >= lineStartOffset());
            generator.emitWillLeaveCallFrameDebugHook();
            generator.emitReturn(r0);
            return;
        }
        break;
    }
    }
}

// ------------------------------ FuncDeclNode ---------------------------------

void FuncDeclNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    generator.hoistSloppyModeFunctionIfNecessary(metadata());
}

// ------------------------------ FuncExprNode ---------------------------------

RegisterID* FuncExprNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    return generator.emitNewFunctionExpression(generator.finalDestination(dst), this);
}

// ------------------------------ ArrowFuncExprNode ---------------------------------

RegisterID* ArrowFuncExprNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    return generator.emitNewArrowFunctionExpression(generator.finalDestination(dst), this);
}

// ------------------------------ MethodDefinitionNode ---------------------------------

RegisterID* MethodDefinitionNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    return generator.emitNewMethodDefinition(generator.finalDestination(dst), this);
}

// ------------------------------ YieldExprNode --------------------------------

RegisterID* YieldExprNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    if (!delegate()) {
        RefPtr<RegisterID> arg = nullptr;
        if (argument()) {
            arg = generator.newTemporary();
            generator.emitNode(arg.get(), argument());
        } else
            arg = generator.emitLoad(nullptr, jsUndefined());
        RefPtr<RegisterID> value = generator.emitYield(arg.get());
        if (dst == generator.ignoredResult())
            return nullptr;
        return generator.move(generator.finalDestination(dst), value.get());
    }
    RefPtr<RegisterID> arg = generator.newTemporary();
    generator.emitNode(arg.get(), argument());
    RefPtr<RegisterID> value = generator.emitDelegateYield(arg.get(), this);
    if (dst == generator.ignoredResult())
        return nullptr;
    return generator.move(generator.finalDestination(dst), value.get());
}

// ------------------------------ AwaitExprNode --------------------------------

RegisterID* AwaitExprNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> arg = generator.newTemporary();
    generator.emitNode(arg.get(), argument());
    return generator.emitAwait(dst ? dst : generator.newTemporary(), arg.get(), position());
}

// ------------------------------ DefineFieldNode ---------------------------------

void DefineFieldNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    RefPtr<RegisterID> value = generator.newTemporary();
    bool shouldSetFunctionName = false;

    if (!m_assign)
        generator.emitLoad(value.get(), jsUndefined());
    else {
        generator.emitNode(value.get(), m_assign);
        shouldSetFunctionName = generator.shouldSetFunctionName(m_assign);
        if (shouldSetFunctionName && m_type != DefineFieldNode::Type::ComputedName)
            generator.emitSetFunctionName(value.get(), m_ident);
    }

    switch (m_type) {
    case DefineFieldNode::Type::Name: {
        StrictModeScope strictModeScope(generator);
        if (auto index = parseIndex(m_ident)) {
            RefPtr<RegisterID> propertyName = generator.emitLoad(nullptr, jsNumber(index.value()));
            generator.emitDirectPutByVal(generator.thisRegister(), propertyName.get(), value.get());
        } else
            generator.emitDirectPutById(generator.thisRegister(), m_ident, value.get());
        break;
    }
    case DefineFieldNode::Type::PrivateName: {
        Variable var = generator.variable(m_ident);
        ASSERT_WITH_MESSAGE(!var.local(), "Private Field names must be stored in captured variables");

        generator.emitExpressionInfo(position(), position(), position() + m_ident.length());
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        RefPtr<RegisterID> privateName = generator.newTemporary();
        generator.emitGetFromScope(privateName.get(), scope.get(), var, DoNotThrowIfNotFound);
        generator.emitDefinePrivateField(generator.thisRegister(), privateName.get(), value.get());
        break;
    }
    case DefineFieldNode::Type::ComputedName: {
        // For ComputedNames, the expression has already been evaluated earlier during evaluation of a ClassExprNode.
        // Here, `m_ident` refers to private symbol ID in a class lexical scope, containing the value already converted to an Expression.
        Variable var = generator.variable(m_ident);
        ASSERT_WITH_MESSAGE(!var.local(), "Computed names must be stored in captured variables");

        generator.emitExpressionInfo(position(), position(), position() + 1);
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        RefPtr<RegisterID> privateName = generator.newTemporary();
        generator.emitGetFromScope(privateName.get(), scope.get(), var, ThrowIfNotFound);
        if (shouldSetFunctionName)
            generator.emitSetFunctionName(value.get(), privateName.get());
        generator.emitProfileType(privateName.get(), var, m_position, m_position + m_ident.length());
        {
            StrictModeScope strictModeScope(generator);
            generator.emitDirectPutByVal(generator.thisRegister(), privateName.get(), value.get());
        }
        break;
    }
    }
}

// ------------------------------ ClassDeclNode ---------------------------------

void ClassDeclNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    generator.emitNode(m_classDeclaration);
}

// ------------------------------ ClassExprNode ---------------------------------

RegisterID* ClassExprNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    StrictModeScope strictModeScope(generator);

    if (!m_name.isNull())
        generator.pushClassHeadLexicalScope(m_classHeadEnvironment);

    // Class heritage must be evaluated outside of private fields access.
    RefPtr<RegisterID> superclass;
    if (m_classHeritage) {
        superclass = generator.newTemporary();
        generator.emitNode(superclass.get(), m_classHeritage);
    }

    if (m_needsLexicalScope)
        generator.pushLexicalScope(this, BytecodeGenerator::ScopeType::ClassScope, BytecodeGenerator::TDZCheckOptimization::Optimize, BytecodeGenerator::NestedScopeType::IsNested);

    bool hasPrivateNames = !!m_lexicalVariables.privateNamesSize();
    bool shouldEmitPrivateBrand = m_lexicalVariables.hasInstancePrivateMethodOrAccessor();
    bool shouldInstallBrandOnConstructor = m_lexicalVariables.hasStaticPrivateMethodOrAccessor();
    if (hasPrivateNames)
        generator.pushPrivateAccessNames(m_lexicalVariables.privateNameEnvironment());
    if (shouldEmitPrivateBrand)
        generator.emitCreatePrivateBrand(m_position, m_position, m_position);

    RefPtr<RegisterID> constructor = generator.tempDestination(dst);
    bool needsHomeObject = false;

    auto needsClassFieldInitializer = this->hasInstanceFields() ? NeedsClassFieldInitializer::Yes : NeedsClassFieldInitializer::No;
    auto privateBrandRequirement = shouldEmitPrivateBrand ? PrivateBrandRequirement::Needed : PrivateBrandRequirement::None;
    if (m_constructorExpression) {
        ASSERT(m_constructorExpression->isFuncExprNode());
        FunctionMetadataNode* metadata = static_cast<FuncExprNode*>(m_constructorExpression)->metadata();
        metadata->setEcmaName(ecmaName());
        metadata->setClassSource(m_classSource);
        metadata->setNeedsClassFieldInitializer(needsClassFieldInitializer == NeedsClassFieldInitializer::Yes);
        metadata->setPrivateBrandRequirement(privateBrandRequirement);
        constructor = generator.emitNode(constructor.get(), m_constructorExpression);
        needsHomeObject = m_classHeritage || metadata->superBinding() == SuperBinding::Needed;
    } else
        constructor = generator.emitNewDefaultConstructor(constructor.get(), m_classHeritage ? ConstructorKind::Extends : ConstructorKind::Base, m_name, ecmaName(), m_classSource, needsClassFieldInitializer, privateBrandRequirement);

    const auto& propertyNames = generator.propertyNames();
    RefPtr<RegisterID> prototype = generator.emitNewObject(generator.newTemporary());

    if (superclass) {
        RefPtr<RegisterID> protoParent = generator.newTemporary();
        generator.emitLoad(protoParent.get(), jsNull());

        Ref<Label> superclassIsNullLabel = generator.newLabel();
        generator.emitJumpIfTrue(generator.emitIsNull(generator.newTemporary(), superclass.get()), superclassIsNullLabel.get());

        Ref<Label> superclassIsConstructorLabel = generator.newLabel();
        generator.emitJumpIfTrue(generator.emitIsConstructor(generator.newTemporary(), superclass.get()), superclassIsConstructorLabel.get());
        generator.emitExpressionInfo(divot(), divotStart(), divotEnd());
        generator.emitThrowTypeError("The superclass is not a constructor."_s);
        generator.emitLabel(superclassIsConstructorLabel.get());
        generator.emitGetById(protoParent.get(), superclass.get(), generator.propertyNames().prototype);

        generator.emitDirectSetPrototypeOf<InvalidPrototypeMode::Throw>(constructor.get(), superclass.get(), m_position, m_position, m_position); // never actually throws
        generator.emitLabel(superclassIsNullLabel.get());
        generator.emitDirectSetPrototypeOf<InvalidPrototypeMode::Throw>(prototype.get(), protoParent.get(), divot(), divotStart(), divotEnd());
    }

    if (needsHomeObject)
        emitPutHomeObject(generator, constructor.get(), prototype.get());

    RefPtr<RegisterID> constructorNameRegister = generator.emitLoad(nullptr, propertyNames.constructor);
    generator.emitCallDefineProperty(prototype.get(), constructorNameRegister.get(), constructor.get(), nullptr, nullptr,
        BytecodeGenerator::PropertyConfigurable | BytecodeGenerator::PropertyWritable, m_position);

    RefPtr<RegisterID> prototypeNameRegister = generator.emitLoad(nullptr, propertyNames.prototype);
    generator.emitCallDefineProperty(constructor.get(), prototypeNameRegister.get(), prototype.get(), nullptr, nullptr, 0, m_position);

    Vector<UnlinkedFunctionExecutable::ClassElementDefinition> staticElementDefinitions;
    if (m_classElements) {
        m_classElements->emitDeclarePrivateFieldNames(generator, generator.scopeRegister());

        Vector<UnlinkedFunctionExecutable::ClassElementDefinition> instanceElementDefinitions;
        generator.emitDefineClassElements(m_classElements, constructor.get(), prototype.get(), instanceElementDefinitions, staticElementDefinitions);
        if (!instanceElementDefinitions.isEmpty()) {
            RefPtr<RegisterID> instanceFieldInitializer = generator.emitNewClassFieldInitializerFunction(generator.newTemporary(), WTFMove(instanceElementDefinitions), m_classHeritage);

            // FIXME: Skip this if the initializer function isn't going to need a home object (no eval or super properties)
            // https://bugs.webkit.org/show_bug.cgi?id=196867
            emitPutHomeObject(generator, instanceFieldInitializer.get(), prototype.get());

            generator.emitDirectPutById(constructor.get(), generator.propertyNames().builtinNames().instanceFieldInitializerPrivateName(), instanceFieldInitializer.get());
        }
    }

    if (!m_name.isNull()) {
        Variable classNameVar = generator.variable(m_name);
        RELEASE_ASSERT(classNameVar.isResolved());
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, classNameVar);
        generator.emitPutToScope(scope.get(), classNameVar, constructor.get(), ThrowIfNotFound, InitializationMode::Initialization);
    }

    if (shouldInstallBrandOnConstructor)
        generator.emitInstallPrivateClassBrand(constructor.get());

    if (!staticElementDefinitions.isEmpty()) {
        RefPtr<RegisterID> staticFieldInitializer = generator.emitNewClassFieldInitializerFunction(generator.newTemporary(), WTFMove(staticElementDefinitions), m_classHeritage);
        // FIXME: Skip this if the initializer function isn't going to need a home object (no eval or super properties)
        // https://bugs.webkit.org/show_bug.cgi?id=196867
        emitPutHomeObject(generator, staticFieldInitializer.get(), constructor.get());

        CallArguments args(generator, nullptr);
        generator.move(args.thisRegister(), constructor.get());
        generator.emitCallIgnoreResult(generator.newTemporary(), staticFieldInitializer.get(), NoExpectedFunction, args, position(), position(), position(), DebuggableCall::No);
    }

    if (hasPrivateNames)
        generator.popPrivateAccessNames();

    if (m_needsLexicalScope)
        generator.popLexicalScope(this);

    if (!m_name.isNull())
        generator.popClassHeadLexicalScope(m_classHeadEnvironment);

    return generator.move(generator.finalDestination(dst, constructor.get()), constructor.get());
}

// ------------------------------ ImportDeclarationNode -----------------------

void ImportDeclarationNode::emitBytecode(BytecodeGenerator&, RegisterID*)
{
    // Do nothing at runtime.
}

// ------------------------------ ExportAllDeclarationNode --------------------

void ExportAllDeclarationNode::emitBytecode(BytecodeGenerator&, RegisterID*)
{
    // Do nothing at runtime.
}

// ------------------------------ ExportDefaultDeclarationNode ----------------

void ExportDefaultDeclarationNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(m_declaration);
    generator.emitNode(dst, m_declaration);
}

// ------------------------------ ExportLocalDeclarationNode ------------------

void ExportLocalDeclarationNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    ASSERT(m_declaration);
    generator.emitNode(dst, m_declaration);
}

// ------------------------------ ExportNamedDeclarationNode ------------------

void ExportNamedDeclarationNode::emitBytecode(BytecodeGenerator&, RegisterID*)
{
    // Do nothing at runtime.
}

// ------------------------------ DestructuringAssignmentNode -----------------
RegisterID* DestructuringAssignmentNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> initializer = generator.tempDestination(dst);
    generator.emitNode(initializer.get(), m_initializer);
    m_bindings->bindValue(generator, initializer.get());
    return generator.move(dst, initializer.get());
}

static void assignDefaultValueIfUndefined(BytecodeGenerator& generator, RegisterID* maybeUndefined, ExpressionNode* defaultValue)
{
    ASSERT(defaultValue);
    Ref<Label> isNotUndefined = generator.newLabel();
    generator.emitJumpIfFalse(generator.emitIsUndefined(generator.newTemporary(), maybeUndefined), isNotUndefined.get());
    generator.emitNode(maybeUndefined, defaultValue);
    generator.emitLabel(isNotUndefined.get());
}

void ArrayPatternNode::bindValue(BytecodeGenerator& generator, RegisterID* rhs) const
{
    RefPtr<RegisterID> iterable = rhs;
    RefPtr<RegisterID> iterator = generator.newTemporary();
    RefPtr<RegisterID> nextOrIndex = generator.newTemporary();
    {
        RefPtr<RegisterID> iteratorSymbol = generator.emitGetById(generator.newTemporary(), iterable.get(), generator.propertyNames().iteratorSymbol);
        CallArguments args(generator, nullptr, 0);
        generator.move(args.thisRegister(), iterable.get());
        generator.emitIteratorOpen(iterator.get(), nextOrIndex.get(), iteratorSymbol.get(), args, this);
    }

    if (m_targetPatterns.isEmpty()) {
        generator.emitIteratorGenericClose(iterator.get(), this);
        return;
    }

    bool bindValueOrDefaultValueCanThrow = m_targetPatterns.containsIf([&](const auto& target) {
        if (target.pattern && target.pattern->bindValueCanThrow(generator))
            return true;

        if (target.defaultValue) {
            if (target.defaultValue->isConstant())
                return false;
            if (target.defaultValue->isResolveNode() && !static_cast<ResolveNode*>(target.defaultValue)->getFromScopeCanThrow(generator))
                return false;
            return true;
        }

        return false;
    });

    RefPtr<RegisterID> done = generator.newTemporary();

    auto emitBindValue = scopedLambda<void(BytecodeGenerator&)>([&](BytecodeGenerator& generator) {
        for (size_t i = 0; i < this->m_targetPatterns.size(); i++) {
            const auto& target = this->m_targetPatterns[i];

            std::optional<BaseAndPropertyName> targetBaseAndPropertyName;
            if (target.pattern && target.pattern->isAssignmentElementNode())
                targetBaseAndPropertyName = static_cast<AssignmentElementNode*>(target.pattern)->emitNodesForDestructuring(generator);

            switch (target.bindingType) {
            case BindingType::Elision:
            case BindingType::Element: {
                Ref<Label> iterationSkipped = generator.newLabel();
                if (i)
                    generator.emitJumpIfTrue(done.get(), iterationSkipped.get());

                RefPtr<RegisterID> value = generator.newTemporary();
                {
                    Ref<Label> valueIsSet = generator.newLabel();
                    CallArguments nextArgs(generator, nullptr, 0);
                    generator.move(nextArgs.thisRegister(), iterator.get());
                    if (bindValueOrDefaultValueCanThrow) {
                        // This implements steps 3-5 of https://tc39.es/ecma262/#sec-iteratornext and similar steps in its callers.
                        // On the fast path, only IteratorNext & friends can throw, resulting in iteratorRecord.[[Done]] being set
                        // to `true` and skipping IteratorClose. As an optimization, we are avoiding emitLoad() here because exception
                        // handlers are not emitted on the fast path and `done` won't be checked in case of an abrupt completion.
                        generator.emitLoad(done.get(), jsBoolean(true));
                    }
                    generator.emitIteratorNext(done.get(), value.get(), iterable.get(), nextOrIndex.get(), nextArgs, this);
                    generator.emitJumpIfFalse(done.get(), valueIsSet.get());
                    generator.emitLabel(iterationSkipped.get());
                    generator.emitLoad(value.get(), jsUndefined());
                    generator.emitLabel(valueIsSet.get());
                }

                if (target.bindingType == BindingType::Element) {
                    if (target.defaultValue)
                        assignDefaultValueIfUndefined(generator, value.get(), target.defaultValue);

                    if (targetBaseAndPropertyName)
                        static_cast<AssignmentElementNode*>(target.pattern)->bindValueWithEmittedNodes(generator, targetBaseAndPropertyName.value(), value.get());
                    else
                        target.pattern->bindValue(generator, value.get());
                }
                break;
            }

            case BindingType::RestElement: {
                RefPtr<RegisterID> array = generator.emitNewArray(generator.newTemporary(), nullptr, 0, ArrayWithUndecided);

                Ref<Label> iterationDone = generator.newLabel();
                if (i)
                    generator.emitJumpIfTrue(done.get(), iterationDone.get());

                RefPtr<RegisterID> index = generator.newTemporary();
                generator.emitLoad(index.get(), jsNumber(0));
                Ref<Label> loopStart = generator.newLabel();
                generator.emitLabel(loopStart.get());

                RefPtr<RegisterID> value = generator.newTemporary();
                {
                    CallArguments nextArgs(generator, nullptr, 0);
                    generator.move(nextArgs.thisRegister(), iterator.get());
                    if (bindValueOrDefaultValueCanThrow) {
                        // This implements steps 3-5 of https://tc39.es/ecma262/#sec-iteratornext and similar steps in its callers.
                        // On the fast path, only IteratorNext & friends can throw, resulting in iteratorRecord.[[Done]] being set
                        // to `true` and skipping IteratorClose. As an optimization, we are avoiding emitLoad() here because exception
                        // handlers are not emitted on the fast path and `done` won't be checked in case of an abrupt completion.
                        generator.emitLoad(done.get(), jsBoolean(true));
                    }
                    generator.emitIteratorNext(done.get(), value.get(), iterable.get(), nextOrIndex.get(), nextArgs, this);
                    generator.emitJumpIfTrue(done.get(), iterationDone.get());
                }

                generator.emitDirectPutByVal(array.get(), index.get(), value.get());
                generator.emitInc(index.get());
                generator.emitJump(loopStart.get());

                generator.emitLabel(iterationDone.get());
                if (targetBaseAndPropertyName)
                    static_cast<AssignmentElementNode*>(target.pattern)->bindValueWithEmittedNodes(generator, targetBaseAndPropertyName.value(), array.get());
                else
                    target.pattern->bindValue(generator, array.get());
                break;
            }
            }
        }
    });

    auto emitIteratorClose = scopedLambda<void(BytecodeGenerator&)>([&](BytecodeGenerator& generator) {
        Ref<Label> iteratorClosed = generator.newLabel();
        generator.emitJumpIfTrue(done.get(), iteratorClosed.get());
        generator.emitIteratorGenericClose(iterator.get(), this);
        generator.emitLabel(iteratorClosed.get());
    });

    if (bindValueOrDefaultValueCanThrow) {
        generator.emitLoad(done.get(), jsBoolean(false));
        generator.emitTryWithFinallyThatDoesNotShadowException(emitBindValue, emitIteratorClose);
    } else {
        emitBindValue(generator);
        emitIteratorClose(generator);
    }
}

void ArrayPatternNode::toString(StringBuilder& builder) const
{
    builder.append('[');
    for (size_t i = 0; i < m_targetPatterns.size(); i++) {
        const auto& target = m_targetPatterns[i];

        switch (target.bindingType) {
        case BindingType::Elision:
            builder.append(',');
            break;

        case BindingType::Element:
            target.pattern->toString(builder);
            if (i < m_targetPatterns.size() - 1)
                builder.append(',');
            break;

        case BindingType::RestElement:
            builder.append("..."_s);
            target.pattern->toString(builder);
            break;
        }
    }
    builder.append(']');
}

void ArrayPatternNode::collectBoundIdentifiers(Vector<Identifier>& identifiers) const
{
    for (size_t i = 0; i < m_targetPatterns.size(); i++) {
        if (DestructuringPatternNode* node = m_targetPatterns[i].pattern)
            node->collectBoundIdentifiers(identifiers);
    }
}

void ObjectPatternNode::toString(StringBuilder& builder) const
{
    builder.append('{');
    for (size_t i = 0; i < m_targetPatterns.size(); i++) {
        if (m_targetPatterns[i].wasString)
            builder.appendQuotedJSONString(m_targetPatterns[i].propertyName.string());
        else
            builder.append(m_targetPatterns[i].propertyName.string());
        builder.append(':');
        m_targetPatterns[i].pattern->toString(builder);
        if (i < m_targetPatterns.size() - 1)
            builder.append(',');
    }
    builder.append('}');
}
    
void ObjectPatternNode::bindValue(BytecodeGenerator& generator, RegisterID* rhs) const
{
    generator.emitRequireObjectCoercible(rhs, "Right side of assignment cannot be destructured"_s);

    BytecodeGenerator::PreservedTDZStack preservedTDZStack;
    generator.preserveTDZStack(preservedTDZStack);

    {
        RefPtr<RegisterID> restElementBase;
        RefPtr<RegisterID> restElementPropertyName;
        RefPtr<RegisterID> newObject;
        IdentifierSet excludedSet;
        std::optional<CallArguments> args;
        unsigned numberOfComputedProperties = 0;
        unsigned indexInArguments = 2;
        if (m_containsRestElement) {
            if (m_containsComputedProperty) {
                for (const auto& target : m_targetPatterns) {
                    if (target.bindingType == BindingType::Element) {
                        if (target.propertyExpression)
                            ++numberOfComputedProperties;
                    }
                }
            }
            restElementBase = generator.newTemporary();
            restElementPropertyName = generator.newTemporary();
            newObject = generator.newTemporary();
            args.emplace(generator, nullptr, indexInArguments + numberOfComputedProperties);
        }

        for (size_t i = 0; i < m_targetPatterns.size(); i++) {
            const auto& target = m_targetPatterns[i];
            if (target.bindingType == BindingType::Element) {
                // If the destructuring becomes get_by_id and mov, then we should store results directly to the local's binding.
                // From
                //     get_by_id          dst:loc10, base:loc9, property:0
                //     mov                dst:loc6, src:loc10
                // To
                //     get_by_id          dst:loc6, base:loc9, property:0
                auto writableDirectBindingIfPossible = [&]() -> RegisterID* {
                    // The following pattern is possible. In that case, after setting |data| local variable, we need to store property name into the set.
                    // So, old property name |data| result must be kept before setting it into |data|.
                    //     ({ [data]: data, ...obj } = object);
                    if (m_containsRestElement && m_containsComputedProperty && target.propertyExpression)
                        return nullptr;
                    // default value can include a reference to local variable. So filling value to a local variable can differ result.
                    // We give up fast path if default value includes non constant.
                    // For example,
                    //     ({ data = data } = object);
                    if (target.defaultValue && !target.defaultValue->isConstant())
                        return nullptr;
                    return target.pattern->writableDirectBindingIfPossible(generator);
                };

                auto finishDirectBindingAssignment = [&]() {
                    ASSERT(writableDirectBindingIfPossible());
                    target.pattern->finishDirectBindingAssignment(generator);
                };

                RefPtr<RegisterID> temp;
                RegisterID* directBinding = writableDirectBindingIfPossible();
                if (directBinding)
                    temp = directBinding;
                else
                    temp = generator.newTemporary();

                std::optional<BaseAndPropertyName> targetBaseAndPropertyName;
                if (!target.propertyExpression) {
                    if (target.pattern->isAssignmentElementNode())
                        targetBaseAndPropertyName = static_cast<AssignmentElementNode*>(target.pattern)->emitNodesForDestructuring(generator);
                    std::optional<uint32_t> optionalIndex = parseIndex(target.propertyName);
                    if (!optionalIndex)
                        generator.emitGetById(temp.get(), rhs, target.propertyName);
                    else {
                        RefPtr<RegisterID> propertyIndex = generator.emitLoad(nullptr, jsNumber(optionalIndex.value()));
                        generator.emitGetByVal(temp.get(), rhs, propertyIndex.get());
                    }
                    if (m_containsRestElement)
                        excludedSet.add(target.propertyName.impl());
                } else {
                    RefPtr<RegisterID> propertyName;
                    if (m_containsRestElement)
                        propertyName = generator.emitNodeForProperty(args->argumentRegister(indexInArguments), target.propertyExpression);
                    else
                        propertyName = generator.emitNodeForProperty(target.propertyExpression);
                    if (!target.propertyExpression->isNumber() && !target.propertyExpression->isString()) {
                        // ToPropertyKey(Number | String) does not have side-effect.
                        // And for Number case, passing it to GetByVal is better for performance.
                        propertyName = generator.emitToPropertyKeyOrNumber(m_containsRestElement ? args->argumentRegister(indexInArguments) : generator.newTemporary(), propertyName.get());
                    }
                    if (m_containsRestElement)
                        indexInArguments++;
                    if (target.pattern->isAssignmentElementNode())
                        targetBaseAndPropertyName = static_cast<AssignmentElementNode*>(target.pattern)->emitNodesForDestructuring(generator);
                    generator.emitGetByVal(temp.get(), rhs, propertyName.get());
                }

                if (target.defaultValue)
                    assignDefaultValueIfUndefined(generator, temp.get(), target.defaultValue);

                if (directBinding) {
                    ASSERT(!targetBaseAndPropertyName);
                    finishDirectBindingAssignment();
                } else if (targetBaseAndPropertyName)
                    static_cast<AssignmentElementNode*>(target.pattern)->bindValueWithEmittedNodes(generator, targetBaseAndPropertyName.value(), temp.get());
                else
                    target.pattern->bindValue(generator, temp.get());
            } else {
                ASSERT(target.bindingType == BindingType::RestElement);
                ASSERT(i == m_targetPatterns.size() - 1);

                std::optional<BaseAndPropertyName> targetBaseAndPropertyName;
                if (target.pattern->isAssignmentElementNode())
                    targetBaseAndPropertyName = static_cast<AssignmentElementNode*>(target.pattern)->emitNodesForDestructuring(generator, restElementBase.get(), restElementPropertyName.get());

                generator.emitNewObject(newObject.get());

                // load and call @copyDataProperties
                RefPtr<RegisterID> copyDataProperties = generator.moveLinkTimeConstant(nullptr, LinkTimeConstant::copyDataProperties);

                // This must be non-tail-call because @copyDataProperties accesses caller-frame.
                generator.move(args->thisRegister(), newObject.get());
                generator.move(args->argumentRegister(0), rhs);
                generator.emitLoad(args->argumentRegister(1), WTFMove(excludedSet));
                generator.emitCallIgnoreResult(generator.newTemporary(), copyDataProperties.get(), NoExpectedFunction, args.value(), divot(), divotStart(), divotEnd(), DebuggableCall::No);

                if (targetBaseAndPropertyName)
                    static_cast<AssignmentElementNode*>(target.pattern)->bindValueWithEmittedNodes(generator, targetBaseAndPropertyName.value(), newObject.get());
                else
                    target.pattern->bindValue(generator, newObject.get());
            }
        }
    }

    generator.restoreTDZStack(preservedTDZStack);
}

void ObjectPatternNode::collectBoundIdentifiers(Vector<Identifier>& identifiers) const
{
    for (size_t i = 0; i < m_targetPatterns.size(); i++)
        m_targetPatterns[i].pattern->collectBoundIdentifiers(identifiers);
}


bool BindingNode::bindValueCanThrow(BytecodeGenerator& generator) const
{
    Variable var = generator.variable(m_boundProperty);
    if (var.offset().isStack() || var.offset().isScope()) {
        if (m_bindingContext != AssignmentContext::ConstDeclarationStatement && var.isReadOnly())
            return true;
        if (m_bindingContext == AssignmentContext::AssignmentExpression && generator.needsTDZCheck(var))
            return true;
        return false;
    }

    return true;
}

RegisterID* BindingNode::writableDirectBindingIfPossible(BytecodeGenerator& generator) const
{
    Variable var = generator.variable(m_boundProperty);
    bool isReadOnly = var.isReadOnly() && m_bindingContext != AssignmentContext::ConstDeclarationStatement;
    if (RegisterID* local = var.local()) {
        if (m_bindingContext == AssignmentContext::AssignmentExpression) {
            if (generator.needsTDZCheck(var))
                return nullptr;
        }
        if (isReadOnly)
            return nullptr;
        return local;
    }
    return nullptr;
}

void BindingNode::finishDirectBindingAssignment(BytecodeGenerator& generator) const
{
    ASSERT(writableDirectBindingIfPossible(generator));
    Variable var = generator.variable(m_boundProperty);
    RegisterID* local = var.local();
    generator.emitProfileType(local, var, divotStart(), divotEnd());
    if (m_bindingContext == AssignmentContext::DeclarationStatement || m_bindingContext == AssignmentContext::ConstDeclarationStatement)
        generator.liftTDZCheckIfPossible(var);
}

void BindingNode::bindValue(BytecodeGenerator& generator, RegisterID* value) const
{
    Variable var = generator.variable(m_boundProperty);
    bool isReadOnly = var.isReadOnly() && m_bindingContext != AssignmentContext::ConstDeclarationStatement;
    if (RegisterID* local = var.local()) {
        if (m_bindingContext == AssignmentContext::AssignmentExpression)
            generator.emitTDZCheckIfNecessary(var, local, nullptr);
        if (isReadOnly) {
            generator.emitReadOnlyExceptionIfNeeded(var);
            return;
        }
        generator.move(local, value);
        generator.emitProfileType(local, var, divotStart(), divotEnd());
        if (m_bindingContext == AssignmentContext::DeclarationStatement || m_bindingContext == AssignmentContext::ConstDeclarationStatement)
            generator.liftTDZCheckIfPossible(var);
        return;
    }
    if (generator.ecmaMode().isStrict())
        generator.emitExpressionInfo(divotEnd(), divotStart(), divotEnd());
    RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
    generator.emitExpressionInfo(divotEnd(), divotStart(), divotEnd());
    if (m_bindingContext == AssignmentContext::AssignmentExpression)
        generator.emitTDZCheckIfNecessary(var, nullptr, scope.get());
    if (isReadOnly) {
        generator.emitReadOnlyExceptionIfNeeded(var);
        return;
    }
    generator.emitPutToScope(scope.get(), var, value, generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, initializationModeForAssignmentContext(m_bindingContext));
    generator.emitProfileType(value, var, divotStart(), divotEnd());
    if (m_bindingContext == AssignmentContext::DeclarationStatement || m_bindingContext == AssignmentContext::ConstDeclarationStatement)
        generator.liftTDZCheckIfPossible(var);
    return;
}

void BindingNode::toString(StringBuilder& builder) const
{
    builder.append(m_boundProperty.string());
}

void BindingNode::collectBoundIdentifiers(Vector<Identifier>& identifiers) const
{
    identifiers.append(m_boundProperty);
}

std::optional<AssignmentElementNode::BaseAndPropertyName> AssignmentElementNode::emitNodesForDestructuring(BytecodeGenerator& generator, RefPtr<RegisterID> base, RefPtr<RegisterID> propertyName) const
{
    if (m_assignmentTarget->isDotAccessorNode()) {
        if (!base)
            base = generator.newTemporary();

        DotAccessorNode* node = static_cast<DotAccessorNode*>(m_assignmentTarget);
        generator.emitNode(base.get(), node->base());
        generator.emitExpressionInfo(divotEnd(), divotStart(), divotEnd());

        return std::pair { base, nullptr };
    }

    if (m_assignmentTarget->isBracketAccessorNode()) {
        if (!base)
            base = generator.newTemporary();
        if (!propertyName)
            propertyName = generator.newTemporary();

        BracketAccessorNode* node = static_cast<BracketAccessorNode*>(m_assignmentTarget);
        generator.emitNode(base.get(), node->base());
        generator.emitNodeForProperty(propertyName.get(), node->subscript());
        generator.emitExpressionInfo(divotEnd(), divotStart(), divotEnd());

        return std::pair { base, propertyName };
    }

    return std::nullopt;
}

void AssignmentElementNode::bindValueWithEmittedNodes(BytecodeGenerator& generator, BaseAndPropertyName pair, RegisterID* value) const
{
    if (m_assignmentTarget->isDotAccessorNode()) {
        DotAccessorNode* node = static_cast<DotAccessorNode*>(m_assignmentTarget);
        node->emitPutProperty(generator, pair.first.get(), value);
        generator.emitProfileType(value, divotStart(), divotEnd());
    } else if (m_assignmentTarget->isBracketAccessorNode()) {
        BracketAccessorNode* node = static_cast<BracketAccessorNode*>(m_assignmentTarget);
        if (node->base()->isSuperNode()) {
            RefPtr<RegisterID> thisValue = generator.ensureThis();
            generator.emitPutByVal(pair.first.get(), thisValue.get(), pair.second.get(), value);
        } else
            generator.emitPutByVal(pair.first.get(), pair.second.get(), value);
        generator.emitProfileType(value, divotStart(), divotEnd());
    }
}

bool AssignmentElementNode::bindValueCanThrow(BytecodeGenerator& generator) const
{
    if (m_assignmentTarget->isResolveNode()) {
        ResolveNode* lhs = static_cast<ResolveNode*>(m_assignmentTarget);
        Variable var = generator.variable(lhs->identifier());
        if (var.offset().isStack() || var.offset().isScope())
            return var.isReadOnly() || generator.needsTDZCheck(var);
    }

    return true;
}

RegisterID* AssignmentElementNode::writableDirectBindingIfPossible(BytecodeGenerator& generator) const
{
    if (!m_assignmentTarget->isResolveNode())
        return nullptr;
    ResolveNode* lhs = static_cast<ResolveNode*>(m_assignmentTarget);
    Variable var = generator.variable(lhs->identifier());
    bool isReadOnly = var.isReadOnly();
    if (RegisterID* local = var.local()) {
        if (generator.needsTDZCheck(var))
            return nullptr;
        if (isReadOnly)
            return nullptr;
        return local;
    }
    return nullptr;
}

void AssignmentElementNode::finishDirectBindingAssignment(BytecodeGenerator& generator) const
{
    ASSERT_UNUSED(generator, writableDirectBindingIfPossible(generator));
    ResolveNode* lhs = static_cast<ResolveNode*>(m_assignmentTarget);
    Variable var = generator.variable(lhs->identifier());
    RegisterID* local = var.local();
    generator.emitProfileType(local, divotStart(), divotEnd());
}

void AssignmentElementNode::collectBoundIdentifiers(Vector<Identifier>&) const
{
}

void AssignmentElementNode::bindValue(BytecodeGenerator& generator, RegisterID* value) const
{
    if (m_assignmentTarget->isResolveNode()) {
        ResolveNode* lhs = static_cast<ResolveNode*>(m_assignmentTarget);
        Variable var = generator.variable(lhs->identifier());
        bool isReadOnly = var.isReadOnly();
        if (RegisterID* local = var.local()) {
            generator.emitTDZCheckIfNecessary(var, local, nullptr);

            if (isReadOnly)
                generator.emitReadOnlyExceptionIfNeeded(var);
            else {
                generator.move(local, value);
                generator.emitProfileType(local, divotStart(), divotEnd());
            }
            return;
        }
        if (generator.ecmaMode().isStrict())
            generator.emitExpressionInfo(divotEnd(), divotStart(), divotEnd());
        RefPtr<RegisterID> scope = generator.emitResolveScope(nullptr, var);
        generator.emitTDZCheckIfNecessary(var, nullptr, scope.get());
        if (isReadOnly) {
            bool threwException = generator.emitReadOnlyExceptionIfNeeded(var);
            if (threwException)
                return;
        }
        generator.emitExpressionInfo(divotEnd(), divotStart(), divotEnd());
        if (!isReadOnly) {
            generator.emitPutToScope(scope.get(), var, value, generator.ecmaMode().isStrict() ? ThrowIfNotFound : DoNotThrowIfNotFound, InitializationMode::NotInitialization);
            generator.emitProfileType(value, var, divotStart(), divotEnd());
        }
    } else if (m_assignmentTarget->isDotAccessorNode()) {
        DotAccessorNode* lhs = static_cast<DotAccessorNode*>(m_assignmentTarget);
        RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(lhs->base(), true, false);
        generator.emitExpressionInfo(divotEnd(), divotStart(), divotEnd());
        lhs->emitPutProperty(generator, base.get(), value);
        generator.emitProfileType(value, divotStart(), divotEnd());
    } else if (m_assignmentTarget->isBracketAccessorNode()) {
        BracketAccessorNode* lhs = static_cast<BracketAccessorNode*>(m_assignmentTarget);
        RefPtr<RegisterID> base = generator.emitNodeForLeftHandSide(lhs->base(), true, false);
        RefPtr<RegisterID> property = generator.emitNodeForLeftHandSideForProperty(lhs->subscript(), true, false);
        generator.emitExpressionInfo(divotEnd(), divotStart(), divotEnd());
        if (lhs->base()->isSuperNode()) {
            RefPtr<RegisterID> thisValue = generator.ensureThis();
            generator.emitPutByVal(base.get(), thisValue.get(), property.get(), value);
        } else
            generator.emitPutByVal(base.get(), property.get(), value);
        generator.emitProfileType(value, divotStart(), divotEnd());
    }
}

void AssignmentElementNode::toString(StringBuilder& builder) const
{
    if (m_assignmentTarget->isResolveNode())
        builder.append(static_cast<ResolveNode*>(m_assignmentTarget)->identifier().string());
}

void RestParameterNode::collectBoundIdentifiers(Vector<Identifier>& identifiers) const
{
    m_pattern->collectBoundIdentifiers(identifiers);
}

void RestParameterNode::toString(StringBuilder& builder) const
{
    builder.append("..."_s);
    m_pattern->toString(builder);
}

void RestParameterNode::bindValue(BytecodeGenerator&, RegisterID*) const
{
    RELEASE_ASSERT_NOT_REACHED();
}

void RestParameterNode::emit(BytecodeGenerator& generator)
{
    RefPtr<RegisterID> temp = generator.newTemporary();
    generator.emitRestParameter(temp.get(), m_numParametersToSkip);
    m_pattern->bindValue(generator, temp.get());
}


RegisterID* SpreadExpressionNode::emitBytecode(BytecodeGenerator&, RegisterID*)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

RegisterID* ObjectSpreadExpressionNode::emitBytecode(BytecodeGenerator& generator, RegisterID* dst)
{
    RefPtr<RegisterID> src = generator.newTemporary();
    generator.emitNode(src.get(), m_expression);
    
    RefPtr<RegisterID> copyDataProperties = generator.moveLinkTimeConstant(nullptr, LinkTimeConstant::copyDataProperties);
    
    CallArguments args(generator, nullptr, 1);
    generator.move(args.thisRegister(), dst);
    generator.move(args.argumentRegister(0), src.get());
    
    // This must be non-tail-call because @copyDataProperties accesses caller-frame.
    generator.emitCallIgnoreResult(generator.newTemporary(), copyDataProperties.get(), NoExpectedFunction, args, divot(), divotStart(), divotEnd(), DebuggableCall::No);
    
    return dst;
}

} // namespace JSC

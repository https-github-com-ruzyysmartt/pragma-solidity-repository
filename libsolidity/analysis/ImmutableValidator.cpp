/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <libsolidity/analysis/ImmutableValidator.h>

#include <libsolutil/CommonData.h>

#include <boost/range/adaptor/reversed.hpp>

using namespace solidity::frontend;

void ImmutableValidator::analyze()
{
	m_readingOfImmutableAllowed = false;
	m_initializationOfImmutableAllowed = true;

	for (VariableDeclaration const* stateVar: m_currentContract.stateVariablesIncludingInherited())
		if (stateVar->value())
		{
			stateVar->value()->accept(*this);
			solAssert(m_initializedStateVariables.emplace(stateVar).second, "");
		}

	auto linearizeContracts = m_currentContract.annotation().linearizedBaseContracts  | boost::adaptors::reversed;

	for (auto const* contract: linearizeContracts)
		if (contract->constructor())
			visitCallable(*contract->constructor());

	m_initializationOfImmutableAllowed = false;

	for (auto const* contract: linearizeContracts)
		for (std::shared_ptr<InheritanceSpecifier> const inheritSpec: contract->baseContracts())
			if (auto args = inheritSpec->arguments())
				ASTNode::listAccept(*args, *this);

	m_readingOfImmutableAllowed = true;

	for (auto const* contract: linearizeContracts)
	{
		for (auto funcDef: contract->definedFunctions())
			visitCallable(*funcDef);

		for (auto modDef: contract->functionModifiers())
			visitCallable(*modDef);
	}

	checkAllVariablesInitialized(m_currentContract.location());
}

bool ImmutableValidator::visit(FunctionDefinition const& _functionDefinition)
{
	return analyseCallable(_functionDefinition);
}

bool ImmutableValidator::visit(ModifierDefinition const& _modifierDefinition)
{
	return analyseCallable(_modifierDefinition);
}

bool ImmutableValidator::visit(MemberAccess const& _memberAccess)
{
	if (
		_memberAccess.memberName() == "selector" &&
		dynamic_cast<FunctionType const*>(_memberAccess.expression().annotation().type) &&
		dynamic_cast<FixedBytesType const*>(_memberAccess.annotation().type)
	)
		return false;

	_memberAccess.expression().accept(*this);

	if (auto varDecl = dynamic_cast<VariableDeclaration const*>(_memberAccess.annotation().referencedDeclaration))
		analyseVariableDeclaration(*varDecl, _memberAccess);
	else if (auto funcType = dynamic_cast<FunctionType const*>(_memberAccess.annotation().type))
		if ((
			funcType->kind() == FunctionType::Kind::Internal ||
			funcType->kind() == FunctionType::Kind::Declaration
		) && funcType->hasDeclaration())
			visitCallable(funcType->declaration());

	return false;
}

bool ImmutableValidator::visit(IfStatement const& _ifStatement)
{
	bool prevInBranch = m_inBranch;

	_ifStatement.condition().accept(*this);

	m_inBranch = true;
	_ifStatement.trueStatement().accept(*this);

	if (auto falseStatement = _ifStatement.falseStatement())
		falseStatement->accept(*this);

	m_inBranch = prevInBranch;

	return false;
}

bool ImmutableValidator::visit(WhileStatement const& _whileStatement)
{
	bool prevInLoop = m_inLoop;
	m_inLoop = true;

	_whileStatement.condition().accept(*this);
	_whileStatement.body().accept(*this);

	m_inLoop = prevInLoop;

	return false;
}

bool ImmutableValidator::visit(Identifier const& _identifier)
{
	if (auto const callableDef = dynamic_cast<CallableDeclaration const*>(_identifier.annotation().referencedDeclaration))
		visitCallable(*findFinalOverride(callableDef));
	if (auto const varDecl = dynamic_cast<VariableDeclaration const*>(_identifier.annotation().referencedDeclaration))
		analyseVariableDeclaration(*varDecl, _identifier);

	return false;
}

bool ImmutableValidator::visit(Return const& _return)
{
	if (m_currentConstructor == nullptr)
		return true;

	if (auto retExpr = _return.expression())
		retExpr->accept(*this);

	checkAllVariablesInitialized(_return.location());

	return false;
}

bool ImmutableValidator::analyseCallable(CallableDeclaration const& _callableDeclaration)
{
	FunctionDefinition const* prevConstructor = m_currentConstructor;
	m_currentConstructor = nullptr;

	if (FunctionDefinition const* funcDef = dynamic_cast<decltype(funcDef)>(&_callableDeclaration))
	{
		if (funcDef->isConstructor())
			m_currentConstructor = funcDef;

		// Disallow init. in the args of mods/base'ctor calls
		bool previousInitAllowed = m_initializationOfImmutableAllowed;
		m_initializationOfImmutableAllowed = false;

		ASTNode::listAccept(funcDef->modifiers(), *this);

		m_initializationOfImmutableAllowed = previousInitAllowed;

		if (funcDef->isImplemented())
			funcDef->body().accept(*this);
	}
	else if (ModifierDefinition const* modDef = dynamic_cast<decltype(modDef)>(&_callableDeclaration))
		modDef->body().accept(*this);

	m_currentConstructor = prevConstructor;

	return false;
}

void ImmutableValidator::analyseVariableDeclaration(VariableDeclaration const& _variableDeclaration, Expression const& _expression)
{
	if (!_variableDeclaration.isStateVariable() || !_variableDeclaration.immutable())
		return;

	if (_expression.annotation().lValueRequested && _expression.annotation().lValueOfOrdinaryAssignment)
	{
		if (!m_currentConstructor)
			m_errorReporter.typeError(
				_expression.location(),
				"Immutable variables can only be initialized inline or directly in the constructor."
			);
		else if (m_currentConstructor->annotation().contract->id() != _variableDeclaration.annotation().contract->id())
			m_errorReporter.typeError(
				_expression.location(),
				"Immutable variables must be initialized in the constructor of the contract they are defined in."
			);
		else if (m_inLoop)
			m_errorReporter.typeError(
				_expression.location(),
				"Immutable variables can only be initialized once, not in a while statement."
			);
		else if (m_inBranch)
			m_errorReporter.typeError(
				_expression.location(),
				"Immutable variables must be initialized unconditionally, not in an if statement."
			);
		else if (!m_initializationOfImmutableAllowed)
			m_errorReporter.typeError(
				_expression.location(),
				"Immutable variables must be initialized in the constructor body."
			);

		if (!m_initializedStateVariables.emplace(&_variableDeclaration).second)
			m_errorReporter.typeError(
				_expression.location(),
				"Immutable state variable already initialized."
			);
	}
	else if (!m_readingOfImmutableAllowed)
		m_errorReporter.typeError(
			_expression.location(),
			"Immutable variables cannot be read during contract creation time, which means they cannot be read in the constructor or any function or modifier called from it."
		);
}

void ImmutableValidator::checkAllVariablesInitialized(langutil::SourceLocation const& _location)
{
	for (VariableDeclaration const* varDecl: m_currentContract.stateVariablesIncludingInherited())
		if (varDecl->immutable())
			if (!util::contains(m_initializedStateVariables, varDecl))
				m_errorReporter.typeError(
					_location,
					langutil::SecondarySourceLocation().append("Not initialized: ", varDecl->location()),
					"Construction controlflow ends without initializing all immutable state variables."
				);
}

void ImmutableValidator::visitCallable(Declaration const& _declaration)
{
	CallableDeclaration const* _callable = dynamic_cast<CallableDeclaration const*>(&_declaration);
	solAssert(_callable != nullptr, "");

	if (m_visitedCallables.emplace(_callable).second)
		_declaration.accept(*this);
}

CallableDeclaration const* ImmutableValidator::findFinalOverride(CallableDeclaration const* _callable)
{
	if (!_callable->virtualSemantics())
		return _callable;

	if (auto originFuncDef = dynamic_cast<FunctionDefinition const*>(_callable))
		for (auto const* contract: m_currentContract.annotation().linearizedBaseContracts)
		{
				for (auto const* funcDef: contract->definedFunctions())
					if (funcDef->name() == originFuncDef->name())
					{
						FunctionTypePointer fpA = FunctionType(*funcDef).asCallableFunction(false);
						FunctionTypePointer fpB = FunctionType(*originFuncDef).asCallableFunction(false);
						if (fpA->hasEqualReturnTypes(*fpB) && fpA->hasEqualParameterTypes(*fpB))
							return funcDef;
					}
		}
	else if (dynamic_cast<ModifierDefinition const*>(_callable))
		for (auto const* contract: m_currentContract.annotation().linearizedBaseContracts)
			for (auto const* modDef: contract->functionModifiers())
				if (_callable->name() == modDef->name())
					return modDef;

	return _callable;
}

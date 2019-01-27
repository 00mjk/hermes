/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes/Parser/JSParser.h"

#include "hermes/Support/PerfSection.h"

#include <sstream>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace hermes {
namespace parser {

// If a function's source code body is smaller than this number of bytes,
// compile it immediately instead of creating a lazy stub.
static const int PreemptiveCompilationThresholdBytes = 160;

/// Declare a RAII recursion tracker. Check whether the recursion limit has
/// been exceeded, and if so generate an error and return an empty
/// llvm::Optional<>.
#define CHECK_RECURSION                \
  TrackRecursion trackRecursion{this}; \
  if (recursionDepthExceeded())        \
    return llvm::None;

JSParser::JSParser(Context &context, std::unique_ptr<llvm::MemoryBuffer> input)
    : context_(context),
      sm_(context.getSourceErrorManager()),
      lexer_(
          std::move(input),
          context.getSourceErrorManager(),
          context.getAllocator(),
          &context.getStringTable(),
          context.isStrictMode()),
      pass_(FullParse) {
  initializeIdentifiers();
}

JSParser::JSParser(Context &context, uint32_t bufferId, ParserPass pass)
    : context_(context),
      sm_(context.getSourceErrorManager()),
      lexer_(
          bufferId,
          context.getSourceErrorManager(),
          context.getAllocator(),
          &context.getStringTable(),
          context.isStrictMode()),
      pass_(pass) {
  preParsed_ = context.getPreParsedBufferInfo(bufferId);
  initializeIdentifiers();
}

void JSParser::initializeIdentifiers() {
  varIdent_ = lexer_.getIdentifier("var");
  getIdent_ = lexer_.getIdentifier("get");
  setIdent_ = lexer_.getIdentifier("set");
  initIdent_ = lexer_.getIdentifier("init");
  useStrictIdent_ = lexer_.getIdentifier("use strict");

  hermesOnlyDirectAccess_ =
      lexer_.getStringTable().getIdentifier("hermes:only-direct-access");
  hermesInitOnce_ = lexer_.getStringTable().getIdentifier("hermes:init-once");

  // Generate the string representation of all tokens.
  for (unsigned i = 0; i != NUM_JS_TOKENS; ++i)
    tokenIdent_[i] = lexer_.getIdentifier(tokenKindStr((TokenKind)i));
}

Optional<ESTree::FileNode *> JSParser::parse() {
  PerfSection parsing("Parsing JavaScript");
  tok_ = lexer_.advance();
  auto res = parseProgram();
  if (!res)
    return None;
  if (lexer_.getSourceMgr().getErrorCount() != 0)
    return None;
  return res.getValue();
}

void JSParser::errorExpected(
    ArrayRef<TokenKind> toks,
    const char *where,
    const char *what,
    SMLoc whatLoc) {
  std::ostringstream ss;

  for (unsigned i = 0; i < toks.size(); ++i) {
    // Insert a separator after the first token.
    if (i > 0) {
      // Use " or " instead of ", " before the last token.
      if (i == toks.size() - 1)
        ss << " or ";
      else
        ss << ", ";
    }
    ss << "'" << tokenKindStr(toks[i]) << "'";
  }

  ss << " expected";

  // Optionally append the 'where' description.
  if (where)
    ss << " " << where;

  SMLoc errorLoc = tok_->getStartLoc();
  SourceErrorManager::SourceCoords curCoords;
  SourceErrorManager::SourceCoords whatCoords;

  // If the location of 'what' is provided, find its and the error's source
  // coordinates.
  if (whatLoc.isValid()) {
    sm_.findBufferLineAndLoc(errorLoc, curCoords);
    sm_.findBufferLineAndLoc(whatLoc, whatCoords);
  }

  if (whatCoords.isSameSourceLineAs(curCoords)) {
    // If the what source coordinates are on the same line as the error, show
    // them both.
    sm_.error(
        errorLoc,
        SourceErrorManager::combineIntoRange(whatLoc, errorLoc),
        ss.str());
  } else {
    sm_.error(errorLoc, ss.str());

    if (what && whatCoords.isValid())
      sm_.note(whatLoc, what);
  }
}

bool JSParser::need(
    TokenKind kind,
    const char *where,
    const char *what,
    SMLoc whatLoc) {
  if (tok_->getKind() == kind) {
    return true;
  }
  errorExpected(kind, where, what, whatLoc);
  return false;
}

bool JSParser::eat(
    TokenKind kind,
    JSLexer::GrammarContext grammarContext,
    const char *where,
    const char *what,
    SMLoc whatLoc) {
  if (need(kind, where, what, whatLoc)) {
    advance(grammarContext);
    return true;
  }
  return false;
}

bool JSParser::checkAndEat(TokenKind kind) {
  if (tok_->getKind() == kind) {
    advance();
    return true;
  }
  return false;
}

bool JSParser::checkAssign() const {
  return checkN(
      TokenKind::equal,
      TokenKind::starequal,
      TokenKind::slashequal,
      TokenKind::percentequal,
      TokenKind::plusequal,
      TokenKind::minusequal,
      TokenKind::lesslessequal,
      TokenKind::greatergreaterequal,
      TokenKind::greatergreatergreaterequal,
      TokenKind::ampequal,
      TokenKind::caretequal,
      TokenKind::pipeequal);
}

bool JSParser::eatSemi(SMLoc &endLoc, bool optional) {
  if (tok_->getKind() == TokenKind::semi) {
    endLoc = tok_->getEndLoc();
    advance();
    return true;
  }

  if (tok_->getKind() == TokenKind::r_brace ||
      tok_->getKind() == TokenKind::eof ||
      lexer_.isNewLineBeforeCurrentToken()) {
    return true;
  }

  if (!optional)
    sm_.error(tok_->getStartLoc(), "';' expected");
  return false;
}

void JSParser::processDirective(UniqueString *directive) {
  if (directive == useStrictIdent_)
    setStrictMode(true);
}

bool JSParser::recursionDepthExceeded() {
  if (recursionDepth_ < MAX_RECURSION_DEPTH)
    return false;
  sm_.error(tok_->getStartLoc(), "Too many nested expressions/statements");
  return true;
}

Optional<ESTree::FileNode *> JSParser::parseProgram() {
  SMLoc startLoc = tok_->getStartLoc();
  ESTree::NodeList stmtList;

  SaveStrictMode saveStrict{this};

  // Parse directives.
  while (auto *dirStmt = parseDirective())
    stmtList.push_back(*dirStmt);

  while (tok_->getKind() != TokenKind::eof) {
    if (tok_->getKind() == TokenKind::rw_function) {
      auto fdecl = parseFunctionDeclaration();
      if (!fdecl)
        return None;

      stmtList.push_back(*fdecl.getValue());
    } else {
      auto stmt = parseStatement();
      if (!stmt)
        return None;

      stmtList.push_back(*stmt.getValue());
    }
  }

  auto *program = setLocation(
      startLoc, tok_, new (context_) ESTree::ProgramNode(std::move(stmtList)));
  program->strictness = ESTree::makeStrictness(isStrictMode());
  return setLocation(
      program, program, new (context_) ESTree::FileNode(program));
}

Optional<ESTree::FunctionDeclarationNode *> JSParser::parseFunctionDeclaration(
    bool forceEagerly) {
  // function
  assert(tok_->getKind() == TokenKind::rw_function);
  SMLoc startLoc = advance().Start;
  // identifier
  if (!need(
          TokenKind::identifier,
          "after 'function'",
          "location of 'function'",
          startLoc))
    return None;

  auto *id = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
  advance();

  // (
  SMLoc lparenLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "before parameter list in function declaration",
          "function declaration location",
          startLoc))
    return None;

  ESTree::NodeList paramList;

  if (tok_->getKind() != TokenKind::r_paren) {
    do {
      if (!need(
              TokenKind::identifier,
              "inside function parameter list",
              "start of parameter list",
              lparenLoc))
        return None;

      auto *ident = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
      advance();

      paramList.push_back(*ident);
    } while (checkAndEat(TokenKind::comma));
  }

  // )
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of function parameter list",
          "function parameter list starts here",
          lparenLoc))
    return None;

  // {
  if (!need(
          TokenKind::l_brace,
          "in function declaration",
          "start of function declaration",
          startLoc))
    return None;
  SaveStrictMode saveStrictMode{this};

  if (pass_ == PreParse) {
    // Create the nodes we want to keep before the AllocationScope.
    auto node = new (context_) ESTree::FunctionDeclarationNode(
        id, nullptr, std::move(paramList), nullptr);
    // Initialize the node with a blank body.
    node->_body = new (context_) ESTree::BlockStatementNode({});

    AllocationScope scope(context_.getAllocator());
    auto body = parseFunctionBody(false, JSLexer::AllowDiv, true);
    if (!body)
      return None;

    node->strictness = ESTree::makeStrictness(isStrictMode());
    return setLocation(startLoc, body.getValue(), node);
  }

  auto parsedBody = parseFunctionBody(forceEagerly, JSLexer::AllowRegExp, true);
  if (!parsedBody)
    return None;
  auto *body = parsedBody.getValue();

  auto *node = setLocation(
      startLoc,
      body,
      new (context_) ESTree::FunctionDeclarationNode(
          id, body, std::move(paramList), nullptr));
  node->strictness = ESTree::makeStrictness(isStrictMode());
  return node;
}

Optional<ESTree::Node *> JSParser::parseStatement() {
  CHECK_RECURSION;

#define _RET(parseFunc)       \
  if (auto res = (parseFunc)) \
    return res.getValue();    \
  else                        \
    return None;

  switch (tok_->getKind()) {
    case TokenKind::l_brace:
      _RET(parseBlock());
    case TokenKind::rw_var:
      _RET(parseVariableStatement());
    case TokenKind::semi:
      _RET(parseEmptyStatement());
    case TokenKind::rw_if:
      _RET(parseIfStatement());
    case TokenKind::rw_while:
      _RET(parseWhileStatement());
    case TokenKind::rw_do:
      _RET(parseDoWhileStatement());
    case TokenKind::rw_for:
      _RET(parseForStatement());
    case TokenKind::rw_continue:
      _RET(parseContinueStatement());
    case TokenKind::rw_break:
      _RET(parseBreakStatement());
    case TokenKind::rw_return:
      _RET(parseReturnStatement());
    case TokenKind::rw_with:
      _RET(parseWithStatement());
    case TokenKind::rw_switch:
      _RET(parseSwitchStatement());
    case TokenKind::rw_throw:
      _RET(parseThrowStatement());
    case TokenKind::rw_try:
      _RET(parseTryStatement());
    case TokenKind::rw_debugger:
      _RET(parseDebuggerStatement());

    default:
      _RET(parseExpressionOrLabelledStatement());
  }

#undef _RET
}

Optional<ESTree::BlockStatementNode *> JSParser::parseFunctionBody(
    bool eagerly,
    JSLexer::GrammarContext grammarContext,
    bool parseDirectives) {
  if (pass_ == LazyParse && !eagerly) {
    auto startLoc = tok_->getStartLoc();
    assert(preParsed_->bodyStartToEnd.count(startLoc) == 1);
    auto endLoc = preParsed_->bodyStartToEnd[startLoc];
    if (endLoc.getPointer() - startLoc.getPointer() >
        PreemptiveCompilationThresholdBytes) {
      lexer_.seek(endLoc);
      advance();

      auto *body = new (context_) ESTree::BlockStatementNode({});
      body->isLazyFunctionBody = true;
      body->bufferId = lexer_.getBufferId();
      return setLocation(startLoc, endLoc, body);
    }
  }

  auto body = parseBlock(grammarContext, parseDirectives);
  if (!body)
    return None;

  if (pass_ == PreParse) {
    preParsed_->bodyStartToEnd[(*body)->getStartLoc()] = (*body)->getEndLoc();
  }

  return body;
}

Optional<ESTree::BlockStatementNode *> JSParser::parseBlock(
    JSLexer::GrammarContext grammarContext,
    bool parseDirectives) {
  // {
  assert(check(TokenKind::l_brace));
  SMLoc startLoc = advance().Start;

  ESTree::NodeList stmtList;

  if (parseDirectives)
    while (auto *dirStmt = parseDirective())
      stmtList.push_back(*dirStmt);

  while (!check(TokenKind::r_brace, TokenKind::eof)) {
    if (tok_->getKind() == TokenKind::rw_function) {
      auto fdecl = parseFunctionDeclaration();
      if (!fdecl)
        return None;

      stmtList.push_back(*fdecl.getValue());
    } else {
      auto stmt = parseStatement();
      if (!stmt)
        return None;

      stmtList.push_back(*stmt.getValue());
    }
  }

  // }
  auto *body = setLocation(
      startLoc,
      tok_,
      new (context_) ESTree::BlockStatementNode(std::move(stmtList)));
  if (!eat(
          TokenKind::r_brace,
          grammarContext,
          "at end of block",
          "block starts here",
          startLoc))
    return None;

  return body;
}

Optional<ESTree::VariableDeclarationNode *> JSParser::parseVariableStatement() {
  assert(check(TokenKind::rw_var));
  SMLoc startLoc = advance().Start;

  ESTree::NodeList declList;
  if (!parseVariableDeclarationList(startLoc, declList))
    return None;

  auto endLoc = declList.back().getEndLoc();
  if (!eatSemi(endLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_)
          ESTree::VariableDeclarationNode(varIdent_, std::move(declList)));
}

Optional<const char *> JSParser::parseVariableDeclarationList(
    SMLoc varLoc,
    ESTree::NodeList &declList,
    bool noIn) {
  do {
    auto optDecl = parseVariableDeclaration(varLoc, noIn);
    if (!optDecl)
      return None;
    declList.push_back(*optDecl.getValue());
  } while (checkAndEat(TokenKind::comma));

  return "OK";
}

Optional<ESTree::VariableDeclaratorNode *> JSParser::parseVariableDeclaration(
    SMLoc varLoc,
    bool noIn) {
  if (!need(
          TokenKind::identifier,
          "in variable declaration",
          "declaration started here",
          varLoc))
    return None;

  auto *id = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
  advance();

  ESTree::VariableDeclaratorNode *node;
  if (check(TokenKind::equal)) {
    auto debugLoc = advance().Start;

    auto expr = parseAssignmentExpression(noIn);
    if (!expr)
      return None;

    node = setLocation(
        id,
        expr.getValue(),
        debugLoc,
        new (context_) ESTree::VariableDeclaratorNode(expr.getValue(), id));
  } else {
    node = setLocation(
        id, id, new (context_) ESTree::VariableDeclaratorNode(nullptr, id));
  }

  return node;
}

Optional<ESTree::EmptyStatementNode *> JSParser::parseEmptyStatement() {
  assert(check(TokenKind::semi));
  auto *empty =
      setLocation(tok_, tok_, new (context_) ESTree::EmptyStatementNode());
  advance();
  return empty;
}

Optional<ESTree::Node *> JSParser::parseExpressionOrLabelledStatement() {
  bool startsWithIdentifier = check(TokenKind::identifier);
  auto optExpr = parseExpression();
  if (!optExpr)
    return None;

  // Check whether this is a label. The expression must have started with an
  // identifier, be just an identifier and be
  // followed by ':'
  if (startsWithIdentifier && isa<ESTree::IdentifierNode>(optExpr.getValue()) &&
      checkAndEat(TokenKind::colon)) {
    auto *id = cast<ESTree::IdentifierNode>(optExpr.getValue());

    auto optBody = parseStatement();
    if (!optBody)
      return None;

    return setLocation(
        id,
        optBody.getValue(),
        new (context_) ESTree::LabeledStatementNode(id, optBody.getValue()));
  } else {
    auto endLoc = optExpr.getValue()->getEndLoc();
    if (!eatSemi(endLoc))
      return None;

    return setLocation(
        optExpr.getValue(),
        endLoc,
        new (context_)
            ESTree::ExpressionStatementNode(optExpr.getValue(), nullptr));
  }
}

Optional<ESTree::IfStatementNode *> JSParser::parseIfStatement() {
  assert(check(TokenKind::rw_if));
  SMLoc startLoc = advance().Start;

  SMLoc condLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'if'",
          "location of 'if'",
          startLoc))
    return None;
  auto optTest = parseExpression();
  if (!optTest)
    return None;
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of 'if' condition",
          "'if' condition starts here",
          condLoc))
    return None;

  auto optConsequent = parseStatement();
  if (!optConsequent)
    return None;

  if (checkAndEat(TokenKind::rw_else)) {
    auto optAlternate = parseStatement();
    if (!optAlternate)
      return None;

    return setLocation(
        startLoc,
        optAlternate.getValue(),
        new (context_) ESTree::IfStatementNode(
            optTest.getValue(),
            optConsequent.getValue(),
            optAlternate.getValue()));
  } else {
    return setLocation(
        startLoc,
        optConsequent.getValue(),
        new (context_) ESTree::IfStatementNode(
            optTest.getValue(), optConsequent.getValue(), nullptr));
  }
}

Optional<ESTree::WhileStatementNode *> JSParser::parseWhileStatement() {
  assert(check(TokenKind::rw_while));
  SMLoc startLoc = advance().Start;

  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'while'",
          "location of 'while'",
          startLoc))
    return None;
  auto optTest = parseExpression();
  if (!optTest)
    return None;
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of 'while' condition",
          "location of 'while'",
          startLoc))
    return None;

  auto optBody = parseStatement();
  if (!optBody)
    return None;

  return setLocation(
      startLoc,
      optBody.getValue(),
      new (context_)
          ESTree::WhileStatementNode(optBody.getValue(), optTest.getValue()));
}

Optional<ESTree::DoWhileStatementNode *> JSParser::parseDoWhileStatement() {
  assert(check(TokenKind::rw_do));
  SMLoc startLoc = advance().Start;

  auto optBody = parseStatement();
  if (!optBody)
    return None;

  SMLoc whileLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::rw_while,
          JSLexer::AllowRegExp,
          "at end of 'do-while'",
          "'do-while' starts here",
          startLoc))
    return None;

  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'do-while'",
          "location of 'while'",
          whileLoc))
    return None;
  auto optTest = parseExpression();
  if (!optTest)
    return None;
  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of 'do-while' condition",
          "location of 'while'",
          whileLoc))
    return None;

  if (!eatSemi(endLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_)
          ESTree::DoWhileStatementNode(optBody.getValue(), optTest.getValue()));
}

Optional<ESTree::Node *> JSParser::parseForStatement() {
  assert(check(TokenKind::rw_for));
  SMLoc startLoc = advance().Start;

  SMLoc lparenLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'for'",
          "location of 'for'",
          startLoc))
    return None;

  ESTree::VariableDeclarationNode *decl = nullptr;
  ESTree::NodePtr expr1 = nullptr;

  if (check(TokenKind::rw_var)) {
    // Productions valid here:
    //   for ( var VariableDeclarationListNoIn
    //   for ( var VariableDeclarationNoIn
    SMLoc varStartLoc = tok_->getStartLoc();
    advance();

    ESTree::NodeList declList;
    if (!parseVariableDeclarationList(varStartLoc, declList, true))
      return None;

    auto endLoc = declList.back().getEndLoc();
    decl = setLocation(
        varStartLoc,
        endLoc,
        new (context_)
            ESTree::VariableDeclarationNode(varIdent_, std::move(declList)));
  } else {
    // Productions valid here:
    //   for ( ExpressionNoInopt
    //   for ( LeftHandSideExpression

    if (!check(TokenKind::semi)) {
      auto optExpr1 = parseExpression(true);
      if (!optExpr1)
        return None;
      expr1 = optExpr1.getValue();
    }
  }

  if (checkAndEat(TokenKind::rw_in)) {
    // Productions valid here:
    //   for ( var VariableDeclarationNoIn in Expression ) Statement
    //   for ( LeftHandSideExpression in Expression ) Statement

    if (decl && decl->_declarations.size() > 1) {
      sm_.error(
          decl->getSourceRange(),
          "Only one binding must be declared in a for-in loop");
      return None;
    }

    auto optRightExpr = parseExpression();

    if (!eat(
            TokenKind::r_paren,
            JSLexer::AllowRegExp,
            "after 'for(... in ...'",
            "location of '('",
            lparenLoc))
      return None;

    auto optBody = parseStatement();
    if (!optBody || !optRightExpr)
      return None;

    return setLocation(
        startLoc,
        optBody.getValue(),
        new (context_) ESTree::ForInStatementNode(
            decl ? decl : expr1, optRightExpr.getValue(), optBody.getValue()));
  } else if (checkAndEat(TokenKind::semi)) {
    // Productions valid here:
    //   for ( var VariableDeclarationListNoIn ; Expressionopt ; Expressionopt )
    //       Statement
    //   for ( ExpressionNoInopt ; Expressionopt ; Expressionopt ) Statement

    ESTree::NodePtr test = nullptr;
    if (!check(TokenKind::semi)) {
      auto optTest = parseExpression();
      if (!optTest)
        return None;
      test = optTest.getValue();
    }

    if (!eat(
            TokenKind::semi,
            JSLexer::AllowRegExp,
            "after 'for( ... ; ...'",
            "location of '('",
            lparenLoc))
      return None;

    ESTree::NodePtr update = nullptr;
    if (!check(TokenKind::r_paren)) {
      auto optUpdate = parseExpression();
      if (!optUpdate)
        return None;
      update = optUpdate.getValue();
    }

    if (!eat(
            TokenKind::r_paren,
            JSLexer::AllowRegExp,
            "after 'for( ... ; ... ; ...'",
            "location of '('",
            lparenLoc))
      return None;

    auto optBody = parseStatement();
    if (!optBody)
      return None;

    return setLocation(
        startLoc,
        optBody.getValue(),
        new (context_) ESTree::ForStatementNode(
            decl ? decl : expr1, test, update, optBody.getValue()));
  } else {
    errorExpected(
        TokenKind::semi,
        TokenKind::rw_in,
        "inside 'for'",
        "location of the 'for'",
        startLoc);
    return None;
  }
}

Optional<ESTree::ContinueStatementNode *> JSParser::parseContinueStatement() {
  assert(check(TokenKind::rw_continue));
  SMRange loc = advance();

  if (eatSemi(loc.End, true))
    return setLocation(
        loc, loc, new (context_) ESTree::ContinueStatementNode(nullptr));

  if (!need(
          TokenKind::identifier,
          "after 'continue'",
          "location of 'continue'",
          loc.Start))
    return None;
  auto *id = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
  advance();

  loc.End = id->getEndLoc();
  if (!eatSemi(loc.End))
    return None;

  return setLocation(
      loc, loc, new (context_) ESTree::ContinueStatementNode(id));
}

Optional<ESTree::BreakStatementNode *> JSParser::parseBreakStatement() {
  assert(check(TokenKind::rw_break));
  SMRange loc = advance();

  if (eatSemi(loc.End, true))
    return setLocation(
        loc, loc, new (context_) ESTree::BreakStatementNode(nullptr));

  if (!need(
          TokenKind::identifier,
          "after 'break'",
          "location of 'break'",
          loc.Start))
    return None;
  auto *id = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
  advance();

  loc.End = id->getEndLoc();
  if (!eatSemi(loc.End))
    return None;

  return setLocation(loc, loc, new (context_) ESTree::BreakStatementNode(id));
}

Optional<ESTree::ReturnStatementNode *> JSParser::parseReturnStatement() {
  assert(check(TokenKind::rw_return));
  SMRange loc = advance();

  if (eatSemi(loc.End, true))
    return setLocation(
        loc, loc, new (context_) ESTree::ReturnStatementNode(nullptr));

  auto optArg = parseExpression();
  if (!optArg)
    return None;

  loc.End = optArg.getValue()->getEndLoc();
  if (!eatSemi(loc.End))
    return None;

  return setLocation(
      loc, loc, new (context_) ESTree::ReturnStatementNode(optArg.getValue()));
}

Optional<ESTree::WithStatementNode *> JSParser::parseWithStatement() {
  assert(check(TokenKind::rw_with));
  SMLoc startLoc = advance().Start;

  SMLoc lparenLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'with'",
          "location of 'with'",
          startLoc))
    return None;

  auto optExpr = parseExpression();
  if (!optExpr)
    return None;

  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "after 'with (...'",
          "location of '('",
          lparenLoc))
    return None;

  auto optBody = parseStatement();
  if (!optBody)
    return None;

  return setLocation(
      startLoc,
      optBody.getValue(),
      new (context_)
          ESTree::WithStatementNode(optExpr.getValue(), optBody.getValue()));
}

Optional<ESTree::SwitchStatementNode *> JSParser::parseSwitchStatement() {
  assert(check(TokenKind::rw_switch));
  SMLoc startLoc = advance().Start;

  SMLoc lparenLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'switch'",
          "location of 'switch'",
          startLoc))
    return None;

  auto optDiscriminant = parseExpression();
  if (!optDiscriminant)
    return None;

  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "after 'switch (...'",
          "location of '('",
          lparenLoc))
    return None;

  SMLoc lbraceLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_brace,
          JSLexer::AllowRegExp,
          "after 'switch (...)'",
          "'switch' starts here",
          startLoc))
    return None;

  ESTree::NodeList clauseList;
  SMLoc defaultLocation; // location of the 'default' clause

  // Parse the switch body.
  while (!check(TokenKind::r_brace)) {
    SMLoc clauseStartLoc = tok_->getStartLoc();

    ESTree::NodePtr testExpr = nullptr;
    bool ignoreClause = false; // Set to true in error recovery when we want to
                               // parse but ignore the parsed statements.
    ESTree::NodeList stmtList;

    SMLoc caseLoc = tok_->getStartLoc();
    if (checkAndEat(TokenKind::rw_case)) {
      auto optTestExpr = parseExpression();
      if (!optTestExpr)
        return None;
      testExpr = optTestExpr.getValue();
    } else if (checkAndEat(TokenKind::rw_default)) {
      if (defaultLocation.isValid()) {
        sm_.error(clauseStartLoc, "more than one 'default' clause in 'switch'");
        sm_.note(defaultLocation, "first 'default' clause was defined here");

        // We want to continue parsing but ignore the statements.
        ignoreClause = true;
      } else {
        defaultLocation = clauseStartLoc;
      }
    } else {
      errorExpected(
          TokenKind::rw_case,
          TokenKind::rw_default,
          "inside 'switch'",
          "location of 'switch'",
          startLoc);
      return None;
    }

    SMLoc colonLoc =
        tok_->getEndLoc(); // save the location in case the clause is empty
    if (!eat(
            TokenKind::colon,
            JSLexer::AllowRegExp,
            "after 'case ...' or 'default'",
            "location of 'case'/'default'",
            caseLoc))
      return None;

    while (!checkN(
        TokenKind::rw_default, TokenKind::rw_case, TokenKind::r_brace)) {
      auto optStmt = parseStatement();
      if (!optStmt)
        return None;
      stmtList.push_back(*optStmt.getValue());
    }

    if (!ignoreClause) {
      auto clauseEndLoc =
          stmtList.empty() ? colonLoc : stmtList.back().getEndLoc();
      clauseList.push_back(*setLocation(
          clauseStartLoc,
          clauseEndLoc,
          new (context_)
              ESTree::SwitchCaseNode(testExpr, std::move(stmtList))));
    }
  }

  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowRegExp,
          "at end of 'switch' statement",
          "location of '{'",
          lbraceLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::SwitchStatementNode(
          optDiscriminant.getValue(), std::move(clauseList)));
}

Optional<ESTree::ThrowStatementNode *> JSParser::parseThrowStatement() {
  assert(check(TokenKind::rw_throw));
  SMLoc startLoc = advance().Start;

  if (lexer_.isNewLineBeforeCurrentToken()) {
    sm_.error(tok_->getStartLoc(), "'throw' argument must be on the same line");
    sm_.note(startLoc, "location of the 'throw'");
    return None;
  }

  auto optExpr = parseExpression();
  if (!optExpr)
    return None;

  SMLoc endLoc = optExpr.getValue()->getEndLoc();
  if (!eatSemi(endLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::ThrowStatementNode(optExpr.getValue()));
}

Optional<ESTree::TryStatementNode *> JSParser::parseTryStatement() {
  assert(check(TokenKind::rw_try));
  SMLoc startLoc = advance().Start;

  if (!need(TokenKind::l_brace, "after 'try'", "location of 'try'", startLoc))
    return None;
  auto optTryBody = parseBlock();
  if (!optTryBody)
    return None;

  ESTree::CatchClauseNode *catchHandler = nullptr;
  ESTree::BlockStatementNode *finallyHandler = nullptr;

  // Parse the optional 'catch' handler.
  SMLoc handlerStartLoc = tok_->getStartLoc();
  if (checkAndEat(TokenKind::rw_catch)) {
    if (!eat(
            TokenKind::l_paren,
            JSLexer::AllowRegExp,
            "after 'catch'",
            "location of 'catch'",
            handlerStartLoc))
      return None;

    if (!need(
            TokenKind::identifier,
            "inside catch list",
            "location of 'catch'",
            handlerStartLoc))
      return None;
    auto *identifier = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
    advance();

    if (!eat(
            TokenKind::r_paren,
            JSLexer::AllowRegExp,
            "after 'catch (...'",
            "location of 'catch'",
            handlerStartLoc))
      return None;

    if (!need(
            TokenKind::l_brace,
            "after 'catch(...)'",
            "location of 'catch'",
            handlerStartLoc))
      return None;
    auto optCatchBody = parseBlock();
    if (!optCatchBody)
      return None;

    catchHandler = setLocation(
        handlerStartLoc,
        optCatchBody.getValue(),
        new (context_)
            ESTree::CatchClauseNode(identifier, optCatchBody.getValue()));
  }

  // Parse the optional 'finally' handler.
  SMLoc finallyLoc = tok_->getStartLoc();
  if (checkAndEat(TokenKind::rw_finally)) {
    if (!need(
            TokenKind::l_brace,
            "after 'finally'",
            "location of 'finally'",
            finallyLoc))
      return None;
    auto optFinallyBody = parseBlock();
    if (!optFinallyBody)
      return None;

    finallyHandler = optFinallyBody.getValue();
  }

  // At least one handler must be present.
  if (!catchHandler && !finallyHandler) {
    errorExpected(
        TokenKind::rw_catch,
        TokenKind::rw_finally,
        "after 'try' block",
        "location of 'try'",
        startLoc);
    return None;
  }

  // Use the last handler's location as the end location.
  SMLoc endLoc =
      finallyHandler ? finallyHandler->getEndLoc() : catchHandler->getEndLoc();
  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::TryStatementNode(
          optTryBody.getValue(), catchHandler, finallyHandler));
}

Optional<ESTree::DebuggerStatementNode *> JSParser::parseDebuggerStatement() {
  assert(check(TokenKind::rw_debugger));
  SMRange loc = advance();

  if (!eatSemi(loc.End))
    return None;

  return setLocation(loc, loc, new (context_) ESTree::DebuggerStatementNode());
}

Optional<ESTree::Node *> JSParser::parsePrimaryExpression() {
  CHECK_RECURSION;

  switch (tok_->getKind()) {
    case TokenKind::rw_this: {
      auto *res =
          setLocation(tok_, tok_, new (context_) ESTree::ThisExpressionNode());
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::identifier: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::rw_null: {
      auto *res =
          setLocation(tok_, tok_, new (context_) ESTree::NullLiteralNode());
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::rw_true:
    case TokenKind::rw_false: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::BooleanLiteralNode(
              tok_->getKind() == TokenKind::rw_true));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::numeric_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::string_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::regexp_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::RegExpLiteralNode(
              tok_->getRegExpLiteral()->getBody(),
              tok_->getRegExpLiteral()->getFlags()));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::l_square: {
      auto res = parseArrayLiteral();
      if (!res)
        return None;
      return res.getValue();
    }

    case TokenKind::l_brace: {
      auto res = parseObjectLiteral();
      if (!res)
        return None;
      return res.getValue();
    }

    case TokenKind::l_paren: {
      SMRange rng = advance();
      auto expr = parseExpression();
      if (!expr)
        return None;
      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowDiv,
              "at end of parenthesized expression",
              "started here",
              rng.Start))
        return None;
      return expr.getValue();
    }

    default:
      sm_.error(tok_->getStartLoc(), "invalid expression");
      return None;
  }
}

Optional<ESTree::ArrayExpressionNode *> JSParser::parseArrayLiteral() {
  assert(check(TokenKind::l_square));
  SMLoc startLoc = advance().Start;

  ESTree::NodeList elemList;

  if (!check(TokenKind::r_square)) {
    for (;;) {
      if (check(TokenKind::comma)) {
        elemList.push_back(
            *setLocation(tok_, tok_, new (context_) ESTree::EmptyNode()));
      } else {
        auto expr = parseAssignmentExpression();
        if (!expr)
          return None;
        elemList.push_back(*expr.getValue());
      }

      if (!checkAndEat(TokenKind::comma))
        break;
      if (check(TokenKind::r_square)) // Check for ",]".
        break;
    }
  }

  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_square,
          JSLexer::AllowDiv,
          "at end of array literal '[...'",
          "location of '['",
          startLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::ArrayExpressionNode(std::move(elemList)));
}

Optional<ESTree::ObjectExpressionNode *> JSParser::parseObjectLiteral() {
  assert(check(TokenKind::l_brace));
  SMLoc startLoc = advance().Start;

  ESTree::NodeList elemList;

  if (!check(TokenKind::r_brace)) {
    for (;;) {
      auto prop = parsePropertyAssignment();
      if (!prop)
        return None;

      elemList.push_back(*prop.getValue());

      if (!checkAndEat(TokenKind::comma))
        break;
      if (check(TokenKind::r_brace)) // check for ",}"
        break;
    }
  }

  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowDiv,
          "at end of object literal '{...'",
          "location of '{'",
          startLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::ObjectExpressionNode(std::move(elemList)));
}

Optional<ESTree::Node *> JSParser::parsePropertyAssignment() {
  SMLoc startLoc = tok_->getStartLoc();
  ESTree::NodePtr key = nullptr;

  SaveStrictMode saveStrictMode{this};

  if (checkIdentifier(getIdent_)) {
    UniqueString *ident = tok_->getResWordOrIdentifier();
    SMRange identRng = tok_->getSourceRange();
    advance();

    // This could either be a getter, or a property named 'get'.
    if (check(TokenKind::colon)) {
      // This is just a property.
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr));
    } else {
      // A getter method.
      auto optKey = parsePropertyKey();
      if (!optKey)
        return None;

      if (!eat(
              TokenKind::l_paren,
              JSLexer::AllowRegExp,
              "in getter declaration",
              "start of getter declaration",
              startLoc))
        return None;
      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowRegExp,
              "in empty getter parameter list",
              "start of getter declaration",
              startLoc))
        return None;
      if (!need(
              TokenKind::l_brace,
              "in getter declaration",
              "start of getter declaration",
              startLoc))
        return None;
      auto block = parseBlock(JSLexer::AllowRegExp, true);
      if (!block)
        return None;

      auto *funcExpr = new (context_) ESTree::FunctionExpressionNode(
          nullptr, ESTree::NodeList{}, block.getValue());
      funcExpr->strictness = ESTree::makeStrictness(isStrictMode());
      setLocation(startLoc, block.getValue(), funcExpr);

      auto *node = new (context_)
          ESTree::PropertyNode(optKey.getValue(), funcExpr, getIdent_);
      return setLocation(startLoc, block.getValue(), node);
    }
  } else if (checkIdentifier(setIdent_)) {
    UniqueString *ident = tok_->getResWordOrIdentifier();
    SMRange identRng = tok_->getSourceRange();
    advance();

    // This could either be a setter, or a property named 'set'.
    if (check(TokenKind::colon)) {
      // This is just a property.
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr));
    } else {
      // A setter method.
      auto optKey = parsePropertyKey();
      if (!optKey)
        return None;

      ESTree::NodeList params;
      eat(TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "in setter declaration",
          "start of setter declaration",
          startLoc);

      if (!need(
              TokenKind::identifier,
              "in setter parameter list",
              "start of setter declaration",
              startLoc))
        return None;
      auto *param = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
      params.push_back(*param);
      advance();

      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowRegExp,
              "at end of setter parameter list",
              "start of setter declaration",
              startLoc))
        return None;
      if (!need(
              TokenKind::l_brace,
              "in setter declaration",
              "start of setter declaration",
              startLoc))
        return None;
      auto block = parseBlock(JSLexer::AllowRegExp, true);
      if (!block)
        return None;

      auto *funcExpr = new (context_) ESTree::FunctionExpressionNode(
          nullptr, std::move(params), block.getValue());
      funcExpr->strictness = ESTree::makeStrictness(isStrictMode());
      setLocation(startLoc, block.getValue(), funcExpr);

      auto *node = new (context_)
          ESTree::PropertyNode(optKey.getValue(), funcExpr, setIdent_);
      return setLocation(startLoc, block.getValue(), node);
    }
  } else {
    auto optKey = parsePropertyKey();
    if (!optKey)
      return None;

    key = optKey.getValue();
  }

  if (!eat(
          TokenKind::colon,
          JSLexer::AllowRegExp,
          "in property initialization",
          "start of property initialization",
          startLoc))
    return None;

  auto value = parseAssignmentExpression();
  if (!value)
    return None;

  return setLocation(
      startLoc,
      value.getValue(),
      new (context_) ESTree::PropertyNode(key, value.getValue(), initIdent_));
}

Optional<ESTree::Node *> JSParser::parsePropertyKey() {
  switch (tok_->getKind()) {
    case TokenKind::string_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
      advance();
      return res;
    }

    case TokenKind::numeric_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
      advance();
      return res;
    }

    case TokenKind::identifier: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
      advance();
      return res;
    }

    default:
      if (tok_->isResWord()) {
        auto *res = setLocation(
            tok_,
            tok_,
            new (context_)
                ESTree::IdentifierNode(tok_->getResWordIdentifier(), nullptr));
        advance();
        return res;
      } else {
        sm_.error(
            tok_->getSourceRange(),
            "invalid property name - must be a string, number of identifier");
        return None;
      }
  }
}

Optional<ESTree::FunctionExpressionNode *> JSParser::parseFunctionExpression(
    bool forceEagerly) {
  assert(check(TokenKind::rw_function));
  SMLoc startLoc = advance().Start;

  ESTree::IdentifierNode *id = nullptr;
  if (check(TokenKind::identifier)) {
    id = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
    advance();
  }

  ESTree::NodeList paramList;
  SMLoc lparenLoc = tok_->getStartLoc();
  eat(TokenKind::l_paren,
      JSLexer::AllowRegExp,
      "at start of function parameter list",
      "function expression starts here",
      startLoc);
  if (!check(TokenKind::r_paren)) {
    do {
      if (!need(
              TokenKind::identifier,
              "inside function parameter list",
              "start of function parameter list",
              lparenLoc))
        return None;
      auto *param = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr));
      advance();
      paramList.push_back(*param);
    } while (checkAndEat(TokenKind::comma));
  }
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of function parameter list",
          "start of function parameter list",
          lparenLoc))
    return None;
  if (!need(
          TokenKind::l_brace,
          "in function expression",
          "start of function expression",
          startLoc))
    return None;
  SaveStrictMode saveStrictMode{this};

  if (pass_ == PreParse) {
    // Create the nodes we want to keep before the AllocationScope.
    auto node = new (context_)
        ESTree::FunctionExpressionNode(id, std::move(paramList), nullptr);
    // Initialize the node with a blank body.
    node->_body = new (context_) ESTree::BlockStatementNode({});

    AllocationScope scope(context_.getAllocator());
    auto body = parseFunctionBody(false, JSLexer::AllowDiv, true);
    if (!body)
      return None;

    node->strictness = ESTree::makeStrictness(isStrictMode());
    return setLocation(startLoc, body.getValue(), node);
  }

  auto body = parseFunctionBody(forceEagerly, JSLexer::AllowDiv, true);
  if (!body)
    return None;

  auto node = new (context_)
      ESTree::FunctionExpressionNode(id, std::move(paramList), body.getValue());
  node->strictness = ESTree::makeStrictness(isStrictMode());
  return setLocation(startLoc, body.getValue(), node);
}

Optional<ESTree::Node *> JSParser::parseMemberExpression() {
  SMLoc startLoc = tok_->getStartLoc();

  ESTree::NodePtr expr;
  if (check(TokenKind::rw_function)) {
    auto fExpr = parseFunctionExpression();
    if (!fExpr)
      return None;
    expr = fExpr.getValue();
  } else if (checkAndEat(TokenKind::rw_new)) {
    auto mExpr = parseMemberExpression();
    if (!mExpr)
      return None;

    auto debugLoc = tok_->getStartLoc();
    if (!need(
            TokenKind::l_paren,
            "after 'new ...'",
            "location of 'new'",
            startLoc))
      return None;
    ESTree::NodeList argList;
    SMLoc endLoc;
    if (!parseArguments(argList, endLoc))
      return None;

    expr = setLocation(
        startLoc,
        endLoc,
        debugLoc,
        new (context_)
            ESTree::NewExpressionNode(mExpr.getValue(), std::move(argList)));
  } else {
    auto primExpr = parsePrimaryExpression();
    if (!primExpr)
      return None;
    expr = primExpr.getValue();
  }

  SMLoc objectLoc = startLoc;
  while (check(TokenKind::l_square, TokenKind::period)) {
    SMLoc nextObjectLoc = tok_->getStartLoc();
    auto msel = parseMemberSelect(objectLoc, expr);
    if (!msel)
      return None;
    objectLoc = nextObjectLoc;
    expr = msel.getValue();
  }

  return expr;
}

Optional<const char *> JSParser::parseArguments(
    ESTree::NodeList &argList,
    SMLoc &endLoc) {
  assert(check(TokenKind::l_paren));
  SMLoc startLoc = advance().Start;
  if (!check(TokenKind::r_paren)) {
    do {
      auto arg = parseAssignmentExpression();
      if (!arg)
        return None;
      argList.push_back(*arg.getValue());
    } while (checkAndEat(TokenKind::comma));
  }
  endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowDiv,
          "at end of function call",
          "location of '('",
          startLoc))
    return None;

  return "OK";
}

Optional<ESTree::Node *> JSParser::parseMemberSelect(
    SMLoc objectLoc,
    ESTree::NodePtr expr) {
  assert(check(TokenKind::l_square, TokenKind::period));
  SMLoc startLoc = tok_->getStartLoc();
  if (checkAndEat(TokenKind::l_square)) {
    auto propExpr = parseExpression();
    if (!propExpr)
      return None;
    SMLoc endLoc = tok_->getEndLoc();
    if (!eat(
            TokenKind::r_square,
            JSLexer::AllowDiv,
            "at end of member expression '[...'",
            "location iof '['",
            startLoc))
      return None;

    return setLocation(
        expr,
        endLoc,
        startLoc,
        new (context_)
            ESTree::MemberExpressionNode(expr, propExpr.getValue(), true));
  } else if (checkAndEat(TokenKind::period)) {
    if (tok_->getKind() != TokenKind::identifier && !tok_->isResWord()) {
      // Just use the pattern here, even though we know it will fail.
      if (!need(
              TokenKind::identifier,
              "after '.' in member expression",
              "start of member expression",
              objectLoc))
        return None;
    }

    auto *id = setLocation(
        tok_,
        tok_,
        new (context_)
            ESTree::IdentifierNode(tok_->getResWordOrIdentifier(), nullptr));
    advance(JSLexer::AllowDiv);

    return setLocation(
        expr,
        id,
        startLoc,
        new (context_) ESTree::MemberExpressionNode(expr, id, false));
  } else {
    assert(false);
    return None;
  }
}

Optional<ESTree::Node *> JSParser::parseCallExpression(
    SMLoc startLoc,
    ESTree::NodePtr expr) {
  assert(check(TokenKind::l_paren));

  for (;;) {
    if (check(TokenKind::l_paren)) {
      auto debugLoc = tok_->getStartLoc();
      ESTree::NodeList argList;
      SMLoc endLoc;
      if (!parseArguments(argList, endLoc))
        return None;

      expr = setLocation(
          expr,
          endLoc,
          debugLoc,
          new (context_) ESTree::CallExpressionNode(expr, std::move(argList)));
    } else if (check(TokenKind::l_square, TokenKind::period)) {
      SMLoc nextStartLoc = tok_->getStartLoc();
      auto msel = parseMemberSelect(startLoc, expr);
      if (!msel)
        return None;
      startLoc = nextStartLoc;
      expr = msel.getValue();
    } else {
      break;
    }
  }

  return expr;
}

Optional<ESTree::Node *> JSParser::parseNewExpression(
    bool &outWasNewExpression) {
  assert(check(TokenKind::rw_new));
  SMLoc startLoc = advance().Start;

  outWasNewExpression = false;

  ESTree::NodePtr expr;
  bool childIsNewExpression;

  if (check(TokenKind::rw_new)) {
    auto nExpr = parseNewExpression(childIsNewExpression);
    if (!nExpr)
      return None;
    expr = nExpr.getValue();
  } else {
    auto mExpr = parseMemberExpression();
    if (!mExpr)
      return None;
    childIsNewExpression = false;
    expr = mExpr.getValue();
  }

  // Do we have arguments to a child MemberExpression? If yes, then it really
  // was a 'new MemberExpression(args)', otherwise it is a NewExpression
  if (childIsNewExpression || !check(TokenKind::l_paren)) {
    outWasNewExpression = true;
    return setLocation(
        startLoc,
        expr,
        new (context_) ESTree::NewExpressionNode(expr, ESTree::NodeList{}));
  }

  auto debugLoc = tok_->getStartLoc();
  ESTree::NodeList argList;
  SMLoc endLoc;
  if (!parseArguments(argList, endLoc))
    return None;

  expr = setLocation(
      startLoc,
      endLoc,
      debugLoc,
      new (context_) ESTree::NewExpressionNode(expr, std::move(argList)));

  SMLoc objectLoc = startLoc;
  while (check(TokenKind::l_square, TokenKind::period)) {
    SMLoc nextObjectLoc = tok_->getStartLoc();
    auto optMSel = parseMemberSelect(objectLoc, expr);
    if (!optMSel)
      return None;
    objectLoc = nextObjectLoc;
    expr = optMSel.getValue();
  }

  return expr;
}

Optional<ESTree::Node *> JSParser::parseLeftHandSideExpression() {
  ESTree::NodePtr expr;
  SMLoc startLoc = tok_->getStartLoc();

  if (check(TokenKind::rw_new)) {
    bool childIsNewExpression;
    auto optNewExpr = parseNewExpression(childIsNewExpression);
    if (!optNewExpr)
      return None;

    expr = optNewExpr.getValue();
    if (childIsNewExpression)
      return expr;
  } else {
    auto optMemberExpr = parseMemberExpression();
    if (!optMemberExpr)
      return None;
    expr = optMemberExpr.getValue();
  }

  // Is this a CallExpression?
  if (check(TokenKind::l_paren)) {
    auto optCallExpr = parseCallExpression(startLoc, expr);
    if (!optCallExpr)
      return None;
    expr = optCallExpr.getValue();
  }

  return expr;
}

Optional<ESTree::Node *> JSParser::parsePostfixExpression() {
  auto optLHandExpr = parseLeftHandSideExpression();
  if (!optLHandExpr)
    return None;

  if (check(TokenKind::plusplus, TokenKind::minusminus) &&
      !lexer_.isNewLineBeforeCurrentToken()) {
    auto *res = setLocation(
        optLHandExpr.getValue(),
        tok_,
        tok_,
        new (context_) ESTree::UpdateExpressionNode(
            getTokenIdent(tok_->getKind()), optLHandExpr.getValue(), false));
    advance(JSLexer::AllowDiv);
    return res;
  } else {
    return optLHandExpr.getValue();
  }
}

Optional<ESTree::Node *> JSParser::parseUnaryExpression() {
  SMLoc startLoc = tok_->getStartLoc();

  switch (tok_->getKind()) {
    case TokenKind::rw_delete:
    case TokenKind::rw_void:
    case TokenKind::rw_typeof:
    case TokenKind::plus:
    case TokenKind::minus:
    case TokenKind::tilde:
    case TokenKind::exclaim: {
      UniqueString *op = getTokenIdent(tok_->getKind());
      advance();
      auto expr = parseUnaryExpression();
      if (!expr)
        return None;

      return setLocation(
          startLoc,
          expr.getValue(),
          new (context_)
              ESTree::UnaryExpressionNode(op, expr.getValue(), true));
    }

    case TokenKind::plusplus:
    case TokenKind::minusminus: {
      UniqueString *op = getTokenIdent(tok_->getKind());
      advance();
      auto expr = parseUnaryExpression();
      if (!expr)
        return None;

      return setLocation(
          startLoc,
          expr.getValue(),
          new (context_)
              ESTree::UpdateExpressionNode(op, expr.getValue(), true));
    }

    default:
      return parsePostfixExpression();
  }
}

namespace {

/// Associates precedence levels with binary operators. Higher precedences are
/// represented by higher values.
/// \returns the precedence level starting from 1, or 0 if not a binop.
inline unsigned getPrecedence(TokenKind kind) {
  // Record the precedence of all binary operators.
  static const unsigned precedence[] = {
#define TOK(...) 0,
#define BINOP(name, str, precedence) precedence,

// There are two reserved words that are binary operators.
#define RESWORD(name)                                       \
  (TokenKind::rw_##name == TokenKind::rw_in ||              \
           TokenKind::rw_##name == TokenKind::rw_instanceof \
       ? 7                                                  \
       : 0),
#include "hermes/Parser/TokenKinds.def"
  };

  return precedence[static_cast<unsigned>(kind)];
}

/// Return the precedence of \p kind unless it happens to be equal to \p except,
/// in which case return 0.
inline unsigned getPrecedenceExcept(TokenKind kind, TokenKind except) {
  return LLVM_LIKELY(kind != except) ? getPrecedence(kind) : 0;
}
} // namespace

Optional<ESTree::Node *> JSParser::parseBinaryExpression(bool noIn) {
  // The stack can never go deeper than the number of precedence levels,
  // and we have 10.
  static const unsigned STACK_SIZE = 16;

  // Operator and value stack.
  ESTree::NodePtr valueStack[STACK_SIZE];
  TokenKind opStack[STACK_SIZE];

  // The stack grows down, because it is more natural to point one past the end
  // of an array, rather than one before.
  unsigned sp = STACK_SIZE;

  // Decide whether to recognize "in" as a binary operator.
  const TokenKind exceptKind = noIn ? TokenKind::rw_in : TokenKind::none;

  auto optExpr = parseUnaryExpression();
  if (!optExpr)
    return None;
  ESTree::NodePtr topExpr = optExpr.getValue();

  // While the current token is a binary operator.
  while (unsigned precedence =
             getPrecedenceExcept(tok_->getKind(), exceptKind)) {
    // If the next operator has lower precedence than the operator on the stack,
    // pop the stack, creating a new binary expression.
    while (sp != STACK_SIZE && precedence <= getPrecedence(opStack[sp])) {
      topExpr = newBinNode(valueStack[sp], opStack[sp], topExpr);
      ++sp;
    }

    // The next operator has a higher precedence than the previous one (or there
    // is no previous one). The situation looks something like this:
    //    .... + topExpr * rightExpr ....
    //                     ^
    //                 We are here
    // Push topExpr and the '*', so we can parse rightExpr.
    --sp;
    opStack[sp] = tok_->getKind();
    advance();

    auto optRightExpr = parseUnaryExpression();
    if (!optRightExpr)
      return None;

    valueStack[sp] = topExpr;
    topExpr = optRightExpr.getValue();
  }

  // We have consumed all binary operators. Pop the stack, creating expressions.
  while (sp != STACK_SIZE) {
    topExpr = newBinNode(valueStack[sp], opStack[sp], topExpr);
    ++sp;
  }

  assert(sp == STACK_SIZE);
  return topExpr;
}

Optional<ESTree::Node *> JSParser::parseConditionalExpression(bool noIn) {
  auto optTest = parseBinaryExpression(noIn);
  if (!optTest)
    return None;

  SMLoc questionLoc = tok_->getStartLoc();
  if (!checkAndEat(TokenKind::question))
    return optTest.getValue();

  auto optConsequent = parseAssignmentExpression(false);
  if (!optConsequent)
    return None;

  if (!eat(
          TokenKind::colon,
          JSLexer::AllowRegExp,
          "in conditional expression after '... ? ...'",
          "location of '?'",
          questionLoc))
    return None;

  auto optAlternate = parseAssignmentExpression(noIn);
  if (!optAlternate)
    return None;

  return setLocation(
      optTest.getValue(),
      optAlternate.getValue(),
      new (context_) ESTree::ConditionalExpressionNode(
          optTest.getValue(),
          optAlternate.getValue(),
          optConsequent.getValue()));
}

Optional<ESTree::Node *> JSParser::parseAssignmentExpression(bool noIn) {
  auto optLeftExpr = parseConditionalExpression(noIn);
  if (!optLeftExpr)
    return None;

  if (!checkAssign())
    return optLeftExpr.getValue();

  UniqueString *op = getTokenIdent(tok_->getKind());
  auto debugLoc = advance().Start;

  auto optRightExpr = parseAssignmentExpression(noIn);
  if (!optRightExpr)
    return None;

  return setLocation(
      optLeftExpr.getValue(),
      optRightExpr.getValue(),
      debugLoc,
      new (context_) ESTree::AssignmentExpressionNode(
          op, optLeftExpr.getValue(), optRightExpr.getValue()));
}

Optional<ESTree::Node *> JSParser::parseExpression(bool noIn) {
  auto optExpr = parseAssignmentExpression(noIn);
  if (!optExpr)
    return None;

  if (!check(TokenKind::comma))
    return optExpr.getValue();

  ESTree::NodeList exprList;
  exprList.push_back(*optExpr.getValue());

  while (checkAndEat(TokenKind::comma)) {
    auto optExpr2 = parseAssignmentExpression(noIn);
    if (!optExpr2)
      return None;

    exprList.push_back(*optExpr2.getValue());
  }

  auto *firstExpr = &exprList.front();
  auto *lastExpr = &exprList.back();
  return setLocation(
      firstExpr,
      lastExpr,
      new (context_) ESTree::SequenceExpressionNode(std::move(exprList)));
}

ESTree::ExpressionStatementNode *JSParser::parseDirective() {
  // Is the current token a directive?
  auto tok = lexer_.rescanCurrentTokenAsDirective();
  if (!tok)
    return nullptr;

  // Save the updated current token. (This is technically a no-op, since the
  // same token is being reused, but that could change in the future.
  tok_ = tok;

  // Allocate a SingLiteralNode for the directive.
  auto *strLit = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::StringLiteralNode(tok_->getDirective()));
  strLit->potentialDirective = true;
  strLit->directive = true;
  auto endLoc = tok_->getEndLoc();

  // Actually process the directive. Note that we want to do that before we
  // have consumed any more tokens - strictness can affect the interpretation
  // of tokens.
  processDirective(strLit->_value);

  advance(JSLexer::AllowDiv);

  // Consume the optional semicolon.
  if (check(TokenKind::semi)) {
    endLoc = tok_->getEndLoc();
    advance();
  }

  // Allocate an ExpressionStatementNode for the directive.
  return setLocation(
      strLit,
      endLoc,
      new (context_) ESTree::ExpressionStatementNode(strLit, nullptr));
}

namespace {
/// Upcast an Optional node type to a generic NodePtr, e.g.
/// \p Optional<FunctionExpressionNode> to \p Optional<NodePtr>.
template <typename T>
Optional<ESTree::NodePtr> castNode(Optional<T> node) {
  if (!node.hasValue())
    return None;
  return Optional<ESTree::NodePtr>(node.getValue());
}
} // namespace

bool JSParser::preParseBuffer(Context &context, uint32_t bufferId) {
  PerfSection preparsing("Pre-Parsing JavaScript");
  AllocationScope scope(context.getAllocator());
  JSParser parser(context, bufferId, PreParse);
  auto result = parser.parse();
  return result.hasValue();
}

Optional<ESTree::NodePtr> JSParser::parseLazyFunction(
    ESTree::NodeKind kind,
    SMLoc start) {
  seek(start);

  switch (kind) {
    case ESTree::NodeKind::FunctionExpression:
      return castNode(parseFunctionExpression(true));

    case ESTree::NodeKind::FunctionDeclaration:
      return castNode(parseFunctionDeclaration(true));

    default:
      llvm_unreachable("Asked to parse unexpected node type");
  }
}

}; // namespace parser
}; // namespace hermes


//
//  file:  rbbiscan.cpp
//
//  Copyright (C) 2002, International Business Machines Corporation and others.
//  All Rights Reserved.
//
//  This file contains the Rule Based Break Iterator Rule Builder functions for
//   scanning the rules and assembling a parse tree.  This is the first phase
//   of compiling the rules.
//
//  The overall of the rules is managed by class RBBIRuleBuilder, which will
//  create and use an instance of this class as part of the process.
//


#include "unicode/unistr.h"
#include "unicode/uniset.h"
#include "unicode/uchar.h"
#include "unicode/uchriter.h"
#include "unicode/parsepos.h"
#include "unicode/parseerr.h"
#include "upropset.h"
#include "cmemory.h"
#include "cstring.h"

#include "rbbirpt.h"   // Contains state table for the rbbi rules parser.
                       //   generated by a Perl script.
#include "rbbirb.h"
#include "rbbinode.h"
#include "rbbiscan.h"

#include "uassert.h"


U_NAMESPACE_BEGIN

const char RBBIRuleScanner::fgClassID=0;

//----------------------------------------------------------------------------------------
//
// Unicode Set init strings for each of the character classes needed for parsing a rule file.
//               (Initialized with hex values for portability to EBCDIC based machines.
//                Really ugly, but there's no good way to avoid it.)
//
//              The sets are referred to by name in the rbbirpt.txt, which is the
//              source form of the state transition table for the RBBI rule parser.
//
//----------------------------------------------------------------------------------------
static const UChar gRuleSet_rule_char_pattern[]       = {
 //   [    ^      [    \     p     {      Z     }     \     u    0      0    2      0
    0x5b, 0x5e, 0x5b, 0x5c, 0x70, 0x7b, 0x5a, 0x7d, 0x5c, 0x75, 0x30, 0x30, 0x32, 0x30,
 //   -    \      u    0     0     7      f     ]     -     [    \      p
    0x2d, 0x5c, 0x75, 0x30, 0x30, 0x37, 0x66, 0x5d, 0x2d, 0x5b, 0x5c, 0x70,
 //   {     L     }    ]     -     [      \     p     {     N    }      ]     ]
    0x7b, 0x4c, 0x7d, 0x5d, 0x2d, 0x5b, 0x5c, 0x70, 0x7b, 0x4e, 0x7d, 0x5d, 0x5d, 0};

static const UChar gRuleSet_name_char_pattern[]       = {
//    [    _      \    p     {     L      }     \     p     {    N      }     ]
    0x5b, 0x5f, 0x5c, 0x70, 0x7b, 0x4c, 0x7d, 0x5c, 0x70, 0x7b, 0x4e, 0x7d, 0x5d, 0};

static const UChar gRuleSet_digit_char_pattern[] = {
//    [    0      -    9     ]
    0x5b, 0x30, 0x2d, 0x39, 0x5d, 0};

static const UChar gRuleSet_name_start_char_pattern[] = {
//    [    _      \    p     {     L      }     ]
    0x5b, 0x5f, 0x5c, 0x70, 0x7b, 0x4c, 0x7d, 0x5d, 0 };

static const UChar kAny[] = {0x61, 0x6e, 0x79, 0x00};  // "any"


U_CDECL_BEGIN
static void  U_EXPORT2 U_CALLCONV RBBISetTable_deleter(void *p) {
    RBBISetTableEl *px = (RBBISetTableEl *)p;
    delete px->key;
    // Note:  px->val is owned by the linked list "fSetsListHead" in scanner.
    //        Don't delete the value nodes here.
    uprv_free(px);
};
U_CDECL_END


//----------------------------------------------------------------------------------------
//
//  Constructor.
//
//----------------------------------------------------------------------------------------
RBBIRuleScanner::RBBIRuleScanner(RBBIRuleBuilder *rb)
{
    fRB                 = rb;
    fStackPtr           = 0;
    fStack[fStackPtr]   = 0;
    fNodeStackPtr       = 0;
    fRuleNum            = 0;
    fNodeStack[0]       = NULL;

    fRuleSets[kRuleSet_rule_char-128]       = NULL;
    fRuleSets[kRuleSet_white_space-128]     = NULL;
    fRuleSets[kRuleSet_name_char-128]       = NULL;
    fRuleSets[kRuleSet_name_start_char-128] = NULL;
    fRuleSets[kRuleSet_digit_char-128]      = NULL;
    fSymbolTable                            = NULL;
    fSetTable                               = NULL;

    fScanIndex = 0;
    fNextIndex = 0;

    fReverseRule        = FALSE;
    fLookAheadRule      = FALSE;

    fLineNum    = 1;
    fCharNum    = 0;
    fQuoteMode  = FALSE;

    if (U_FAILURE(*rb->fStatus)) {
        return;
    }

    //
    //  Set up the constant Unicode Sets.
    //     Note:  These could be made static, lazily initialized, and shared among
    //            all instances of RBBIRuleScanners.  BUT this is quite a bit simpler,
    //            and the time to build these few sets should be small compared to a
    //            full break iterator build.
    fRuleSets[kRuleSet_rule_char-128]       = new UnicodeSet(gRuleSet_rule_char_pattern,       *rb->fStatus);
    fRuleSets[kRuleSet_white_space-128]     = new UnicodeSet(UnicodePropertySet::getRuleWhiteSpaceSet(*rb->fStatus));
    fRuleSets[kRuleSet_name_char-128]       = new UnicodeSet(gRuleSet_name_char_pattern,       *rb->fStatus);
    fRuleSets[kRuleSet_name_start_char-128] = new UnicodeSet(gRuleSet_name_start_char_pattern, *rb->fStatus);
    fRuleSets[kRuleSet_digit_char-128]      = new UnicodeSet(gRuleSet_digit_char_pattern,      *rb->fStatus);
    if (U_FAILURE(*rb->fStatus)) {
        return;
    }

    fSymbolTable = new RBBISymbolTable(this, rb->fRules, *rb->fStatus);
    fSetTable    = uhash_open(uhash_hashUnicodeString, uhash_compareUnicodeString, rb->fStatus);
    uhash_setValueDeleter(fSetTable, RBBISetTable_deleter);
}



//----------------------------------------------------------------------------------------
//
//  Destructor
//
//----------------------------------------------------------------------------------------
RBBIRuleScanner::~RBBIRuleScanner() {
    delete fRuleSets[kRuleSet_rule_char-128];
    delete fRuleSets[kRuleSet_white_space-128];
    delete fRuleSets[kRuleSet_name_char-128];
    delete fRuleSets[kRuleSet_name_start_char-128];
    delete fRuleSets[kRuleSet_digit_char-128];

    delete fSymbolTable;
    if (fSetTable != NULL) {
         uhash_close(fSetTable);
         fSetTable = NULL;

    }


    // Node Stack.
    //   Normally has one entry, which is the entire parse tree for the rules.
    //   If errors occured, there may be additional subtrees left on the stack.
    while (fNodeStackPtr > 0) {
        delete fNodeStack[fNodeStackPtr];
        fNodeStackPtr--;
    }

}

//----------------------------------------------------------------------------------------
//
//  doParseAction        Do some action during rule parsing.
//                       Called by the parse state machine.
//                       Actions build the parse tree and Unicode Sets,
//                       and maintain the parse stack for nested expressions.
//
//                       TODO:  unify EParseAction and RBBI_RuleParseAction enum types.
//                              They represent exactly the same thing.  They're separate
//                              only to work around enum forward declaration restrictions
//                              in some compilers, while at the same time avoiding multiple
//                              definitions problems.  I'm sure that there's a better way.
//
//----------------------------------------------------------------------------------------
UBool RBBIRuleScanner::doParseActions(EParseAction action)
{
    RBBINode *n       = NULL;

    UBool   returnVal = TRUE;

    switch ((RBBI_RuleParseAction)action) {

    case doExprStart:
        pushNewNode(RBBINode::opStart);
        fRuleNum++;
        break;


    case doExprOrOperator:
        {
            fixOpStack(RBBINode::precOpCat);
            RBBINode  *operandNode = fNodeStack[fNodeStackPtr--];
            RBBINode  *orNode      = pushNewNode(RBBINode::opOr);
            orNode->fLeftChild     = operandNode;
            operandNode->fParent   = orNode;
        }
        break;

    case doExprCatOperator:
        // concatenation operator.
        // For the implicit concatenation of adjacent terms in an expression that are
        //   not separated by any other operator.  Action is invoked between the
        //   actions for the two terms.
        {
            fixOpStack(RBBINode::precOpCat);
            RBBINode  *operandNode = fNodeStack[fNodeStackPtr--];
            RBBINode  *catNode     = pushNewNode(RBBINode::opCat);
            catNode->fLeftChild    = operandNode;
            operandNode->fParent   = catNode;
        }
        break;

    case doLParen:
        // Open Paren.
        //   The openParen node is a dummy operation type with a low precedence,
        //     which has the affect of ensuring that any real binary op that
        //     follows within the parens binds more tightly to the operands than
        //     stuff outside of the parens.
        pushNewNode(RBBINode::opLParen);
        break;

    case doExprRParen:
        fixOpStack(RBBINode::precLParen);
        break;

    case doNOP:
        break;

    case doStartAssign:
        // We've just scanned "$variable = "
        // The top of the node stack has the $variable ref node.

        // Save the start position of the RHS text in the StartExpression node
        //   that precedes the $variableReference node on the stack.
        //   This will eventually be used when saving the full $variable replacement
        //   text as a string.
        n = fNodeStack[fNodeStackPtr-1];
        n->fFirstPos = fNextIndex;              // move past the '='

        // Push a new start-of-expression node; needed to keep parse of the
        //   RHS expression happy.
        pushNewNode(RBBINode::opStart);
        break;




    case doEndAssign:
        {
            // We have reached the end of an assignement statement.
            //   Current scan char is the ';' that terminates the assignment.

            // Terminate expression, leaves expression parse tree rooted in TOS node.
            fixOpStack(RBBINode::precStart);

            RBBINode *startExprNode  = fNodeStack[fNodeStackPtr-2];
            RBBINode *varRefNode     = fNodeStack[fNodeStackPtr-1];
            RBBINode *RHSExprNode    = fNodeStack[fNodeStackPtr];

            // Save original text of right side of assignment, excluding the terminating ';'
            //  in the root of the node for the right-hand-side expression.
            RHSExprNode->fFirstPos = startExprNode->fFirstPos;
            RHSExprNode->fLastPos  = fScanIndex;
            fRB->fRules.extractBetween(RHSExprNode->fFirstPos, RHSExprNode->fLastPos, RHSExprNode->fText);

            // Expression parse tree becomes l. child of the $variable reference node.
            varRefNode->fLeftChild = RHSExprNode;
            RHSExprNode->fParent   = varRefNode;

            // Make a symbol table entry for the $variableRef node.
            fSymbolTable->addEntry(varRefNode->fText, varRefNode, *fRB->fStatus);

            // Clean up the stack.
            delete startExprNode;
            fNodeStackPtr-=3;
            break;
        }

    case doEndOfRule:
        {
        fixOpStack(RBBINode::precStart);      // Terminate expression, leaves expression
        if (U_FAILURE(*fRB->fStatus)) {       //   parse tree rooted in TOS node.
            break;
        }
        if (fRB->fDebugEnv && uprv_strstr(fRB->fDebugEnv, "rtree")) {printNodeStack("end of rule");}
        U_ASSERT(fNodeStackPtr == 1);

        // If this rule includes a look-ahead '/', add a endMark node to the
        //   expression tree.
        if (fLookAheadRule) {
            RBBINode  *thisRule       = fNodeStack[fNodeStackPtr];
            RBBINode  *endNode        = pushNewNode(RBBINode::endMark);
            RBBINode  *catNode        = pushNewNode(RBBINode::opCat);
            fNodeStackPtr -= 2;
            catNode->fLeftChild       = thisRule;
            catNode->fRightChild      = endNode;
            fNodeStack[fNodeStackPtr] = catNode;
            endNode->fVal             = fRuleNum;
            endNode->fLookAheadEnd    = TRUE;
        }

        // All rule expressions are ORed together.
        // The ';' that terminates an expression really just functions as a '|' with
        //   a low operator prededence.
        //
        // Forward and reverse rules are collected separately.  Or this rule into
        //  the appropriate group of them.
        //
        RBBINode **destRules = (fReverseRule? &fRB->fReverseTree : &fRB->fForwardTree);

        if (*destRules != NULL) {
            // This is not the first rule encounted.
            // OR previous stuff  (from *destRules)
            // with the current rule expression (on the Node Stack)
            //  with the resulting OR expression going to *destRules
            //
            RBBINode  *thisRule    = fNodeStack[fNodeStackPtr];
            RBBINode  *prevRules   = *destRules;
            RBBINode  *orNode      = pushNewNode(RBBINode::opOr);
            orNode->fLeftChild     = prevRules;
            prevRules->fParent     = orNode;
            orNode->fRightChild    = thisRule;
            thisRule->fParent      = orNode;
            *destRules             = orNode;
        }
        else
        {
            // This is the first rule encountered (for this direction).
            // Just move its parse tree from the stack to *destRules.
            *destRules = fNodeStack[fNodeStackPtr];
        }
        fReverseRule   = FALSE;   // in preparation for the next rule.
        fLookAheadRule = FALSE;
        fNodeStackPtr  = 0;
        }
        break;


    case doRuleError:
        error(U_BRK_RULE_SYNTAX);
        returnVal = FALSE;
        break;


    case doVariableNameExpectedErr:
        error(U_BRK_RULE_SYNTAX);
        break;


    //
    //  Unary operands  + ? *
    //    These all appear after the operand to which they apply.
    //    When we hit one, the operand (may be a whole sub expression)
    //    will be on the top of the stack.
    //    Unary Operator becomes TOS, with the old TOS as its one child.
    case doUnaryOpPlus:
        {
            RBBINode  *operandNode = fNodeStack[fNodeStackPtr--];
            RBBINode  *plusNode    = pushNewNode(RBBINode::opPlus);
            plusNode->fLeftChild   = operandNode;
            operandNode->fParent   = plusNode;
        }
        break;

    case doUnaryOpQuestion:
        {
            RBBINode  *operandNode = fNodeStack[fNodeStackPtr--];
            RBBINode  *qNode       = pushNewNode(RBBINode::opQuestion);
            qNode->fLeftChild      = operandNode;
            operandNode->fParent   = qNode;
        }
        break;

    case doUnaryOpStar:
        {
            RBBINode  *operandNode = fNodeStack[fNodeStackPtr--];
            RBBINode  *starNode    = pushNewNode(RBBINode::opStar);
            starNode->fLeftChild   = operandNode;
            operandNode->fParent   = starNode;
        }
        break;

    case doRuleChar:
        // A "Rule Character" is any single character that is a literal part
        // of the regular expression.  Like a, b and c in the expression "(abc*) | [:L:]"
        // These are pretty uncommon in break rules; the terms are more commonly
        //  sets.  To keep things uniform, treat these characters like as
        // sets that just happen to contain only one character.
        {
            n = pushNewNode(RBBINode::setRef);
            findSetFor(fC.fChar, n);
            n->fFirstPos = fScanIndex;
            n->fLastPos  = fNextIndex;
            fRB->fRules.extractBetween(n->fFirstPos, n->fLastPos, n->fText);
            break;
        }

    case doDotAny:
        // scanned a ".", meaning match any single character.
        {
            n = pushNewNode(RBBINode::setRef);
            findSetFor(kAny, n);
            n->fFirstPos = fScanIndex;
            n->fLastPos  = fNextIndex;
            fRB->fRules.extractBetween(n->fFirstPos, n->fLastPos, n->fText);
            break;
        }
        break;

    case doSlash:
        // Scanned a '/', which identifies a look-ahead break position in a rule.
        n = pushNewNode(RBBINode::lookAhead);
        n->fVal      = fRuleNum;
        n->fFirstPos = fScanIndex;
        n->fLastPos  = fNextIndex;
        fRB->fRules.extractBetween(n->fFirstPos, n->fLastPos, n->fText);
        fLookAheadRule = TRUE;
        break;


    case doStartTagValue:
        // Scanned a '{', the opening delimiter for a tag value within a rule.
        n = pushNewNode(RBBINode::tag);
        n->fVal      = 0;
        n->fFirstPos = fScanIndex;
        n->fLastPos  = fNextIndex;
        break;

    case doTagDigit:
        // Just scanned a decimal digit that's part of a tag value
        {
            n = fNodeStack[fNodeStackPtr];
            uint32_t v = u_charDigitValue(fC.fChar);
            U_ASSERT(v < 10);
            n->fVal = n->fVal*10 + v;
            break;
        }

    case doTagValue:
        n = fNodeStack[fNodeStackPtr];
        n->fLastPos = fNextIndex;
        fRB->fRules.extractBetween(n->fFirstPos, n->fLastPos, n->fText);
        break;



    case doReverseDir:
        fReverseRule = TRUE;
        break;

    case doStartVariableName:
        n = pushNewNode(RBBINode::varRef);
        if (U_FAILURE(*fRB->fStatus)) {break;};
        n->fFirstPos = fScanIndex;
        break;

    case doEndVariableName:
        n = fNodeStack[fNodeStackPtr];
        if (n==NULL || n->fType != RBBINode::varRef) {
            error(U_BRK_INTERNAL_ERROR);
            break;
        }
        n->fLastPos = fScanIndex;
        fRB->fRules.extractBetween(n->fFirstPos+1, n->fLastPos, n->fText);
        // Look the newly scanned name up in the symbol table
        //   If there's an entry, set the l. child of the var ref to the replacement expression.
        //   (We also pass through here when scanning assignments, but no harm is done, other
        //    than a slight wasted effort that seems hard to avoid.  Lookup will be null)
        n->fLeftChild = fSymbolTable->lookupNode(n->fText);
        break;

    case doCheckVarDef:
        n = fNodeStack[fNodeStackPtr];
        if (n->fLeftChild == NULL) {
            error(U_BRK_UNDEFINED_VARIABLE);
            returnVal = FALSE;
        }
        break;

    case doExprFinished:
        break;

    case doRuleErrorAssignExpr:
        error(U_BRK_ASSIGN_ERROR);
        returnVal = FALSE;
        break;

    case doExit:
        returnVal = FALSE;
        break;

    case doScanUnicodeSet:
        scanSet();
        break;

    default:
        error(U_BRK_INTERNAL_ERROR);
        returnVal = FALSE;
        break;
    }
    return returnVal;
};




//----------------------------------------------------------------------------------------
//
//  Error         Report a rule parse error.
//                Only report it if no previous error has been recorded.
//
//----------------------------------------------------------------------------------------
void RBBIRuleScanner::error(UErrorCode e) {
    if (U_SUCCESS(*fRB->fStatus)) {
        *fRB->fStatus = e;
        fRB->fParseError->line  = fLineNum;
        fRB->fParseError->offset = fCharNum;
        fRB->fParseError->preContext[0] = 0;
        fRB->fParseError->preContext[0] = 0;
    }
}




//----------------------------------------------------------------------------------------
//
//  fixOpStack   The parse stack holds partially assembled chunks of the parse tree.
//               An entry on the stack may be as small as a single setRef node,
//               or as large as the parse tree
//               for an entire expression (this will be the one item left on the stack
//               when the parsing of an RBBI rule completes.
//
//               This function is called when a binary operator is encountered.
//               It looks back up the stack for operators that are not yet associated
//               with a right operand, and if the precedence of the stacked operator >=
//               the precedence of the current operator, binds the operand left,
//               to the previously encountered operator.
//
//----------------------------------------------------------------------------------------
void RBBIRuleScanner::fixOpStack(RBBINode::OpPrecedence p) {
    RBBINode *n;
    // printNodeStack("entering fixOpStack()");
    for (;;) {
        n = fNodeStack[fNodeStackPtr-1];   // an operator node
        if (n->fPrecedence == 0) {
            RBBIDebugPrintf("RBBIRuleScanner::fixOpStack, bad operator node\n");
            error(U_BRK_INTERNAL_ERROR);
            return;
        }

        if (n->fPrecedence < p || n->fPrecedence <= RBBINode::precLParen) {
            // The most recent operand goes with the current operator,
            //   not with the previously stacked one.
            break;
        }
            // Stack operator is a binary op  ( '|' or concatenation)
            //   TOS operand becomes right child of this operator.
            //   Resulting subexpression becomes the TOS operand.
            n->fRightChild = fNodeStack[fNodeStackPtr];
            fNodeStack[fNodeStackPtr]->fParent = n;
            fNodeStackPtr--;
        // printNodeStack("looping in fixOpStack()   ");
    }

    if (p <= RBBINode::precLParen) {
        // Scan is at a right paren or end of expression.
        //  The scanned item must match the stack, or else there was an error.
        //  Discard the left paren (or start expr) node from the stack,
            //  leaving the completed (sub)expression as TOS.
            if (n->fPrecedence != p) {
                // Right paren encountered matched start of expression node, or
                // end of expression matched with a left paren node.
                error(U_BRK_MISMATCHED_PAREN);
            }
            fNodeStack[fNodeStackPtr-1] = fNodeStack[fNodeStackPtr];
            fNodeStackPtr--;
            // Delete the now-discarded LParen or Start node.
            delete n;
    }
    // printNodeStack("leaving fixOpStack()");
}




//----------------------------------------------------------------------------------------
//
//   findSetFor    given a UnicodeString,
//                  - find the corresponding Unicode Set  (uset node)
//                         (create one if necessary)
//                  - Set fLeftChild of the caller's node (should be a setRef node)
//                         to the uset node
//                 Maintain a hash table of uset nodes, so the same one is always used
//                    for the same string.
//                 If a "to adopt" set is provided and we haven't seen this key before,
//                    add the provided set to the hash table.
//                 If the string is one (32 bit) char in length, the set contains
//                    just one element which is the char in question.
//                 If the string is "any", return a set containing all chars.
//
//----------------------------------------------------------------------------------------
void RBBIRuleScanner::findSetFor(const UnicodeString &s, RBBINode *node, UnicodeSet *setToAdopt) {

    RBBISetTableEl   *el;

    // First check whether we've already cached a set for this string.
    // If so, just use the cached set in the new node.
    //   delete any set provided by the caller, since we own it.
    el = (RBBISetTableEl *)uhash_get(fSetTable, &s);
    if (el != NULL) {
        delete setToAdopt;
        node->fLeftChild = el->val;
        U_ASSERT(node->fLeftChild->fType == RBBINode::uset);
        return;
    }

    // Haven't seen this set before.
    // If the caller didn't provide us with a prebuilt set,
    //   create a new UnicodeSet now.
    if (setToAdopt == NULL) {
        if (s.compare(kAny, -1) == 0) {
            setToAdopt = new UnicodeSet(0x000000, 0x10ffff);
        } else {
            UChar32 c;
            c = s.char32At(0);
            setToAdopt = new UnicodeSet(c, c);
        }
    }

    //
    // Make a new uset node to refer to this UnicodeSet
    // This new uset node becomes the child of the caller's setReference node.
    //
    RBBINode *usetNode    = new RBBINode(RBBINode::uset);
    usetNode->fInputSet   = setToAdopt;
    usetNode->fParent     = node;
    node->fLeftChild      = usetNode;
    usetNode->fText = s;


    //
    // Add the new uset node to the list of all uset nodes.
    //
    fRB->fUSetNodes->addElement(usetNode, *fRB->fStatus);


    //
    // Add the new set to the set hash table.
    //
    el      = (RBBISetTableEl *)uprv_malloc(sizeof(RBBISetTableEl));
    UnicodeString *tkey = new UnicodeString(s);
    if (tkey == NULL || el == NULL || setToAdopt == NULL) {
        error(U_MEMORY_ALLOCATION_ERROR);
        return;
    }
    el->key = tkey;
    el->val = usetNode;
    uhash_put(fSetTable, el->key, el, fRB->fStatus);

    return;
}



//
//  Assorted Unicode character constants.
//     Numeric because there is no portable way to enter them as literals.
//     (Think EBCDIC).
//
static const UChar      chCR        = 0x0d;      // New lines, for terminating comments.
static const UChar      chLF        = 0x0a;
static const UChar      chNEL       = 0x85;      //    NEL newline variant
static const UChar      chLS        = 0x2028;    //    Unicode Line Separator
static const UChar      chApos      = 0x27;      //  single quote, for quoted chars.
static const UChar      chPound     = 0x23;      // '#', introduces a comment.
static const UChar      chBackSlash = 0x5c;      // '\'  introduces a char escape
static const UChar      chLParen    = 0x28;
static const UChar      chRParen    = 0x29;


//----------------------------------------------------------------------------------------
//
//  nextCharLL    Low Level Next Char from rule input source.
//                Get a char from the input character iterator,
//                keep track of input position for error reporting.
//
//----------------------------------------------------------------------------------------
UChar32  RBBIRuleScanner::nextCharLL() {
    UChar32  ch;

    if (fNextIndex >= fRB->fRules.length()) {
        return (UChar32)-1;
    }
    ch         = fRB->fRules.char32At(fNextIndex);
    fNextIndex = fRB->fRules.moveIndex32(fNextIndex, 1);

    if (ch == chCR ||
        ch == chNEL ||
        ch == chLS   ||
        ch == chLF && fLastChar != chCR) {
        // Character is starting a new line.  Bump up the line number, and
        //  reset the column to 0.
        fLineNum++;
        fCharNum=0;
        if (fQuoteMode) {
            error(U_BRK_NEW_LINE_IN_QUOTED_STRING);
            fQuoteMode = FALSE;
        }
    }
    else {
        // Character is not starting a new line.  Except in the case of a
        //   LF following a CR, increment the column position.
        if (ch != chLF) {
            fCharNum++;
        }
    }
    fLastChar = ch;
    return ch;
}


//---------------------------------------------------------------------------------
//
//   nextChar     for rules scanning.  At this level, we handle stripping
//                out comments and processing backslash character escapes.
//                The rest of the rules grammar is handled at the next level up.
//
//---------------------------------------------------------------------------------
void RBBIRuleScanner::nextChar(RBBIRuleChar &c) {

    // Unicode Character constants needed for the processing done by nextChar(),
    //   in hex because literals wont work on EBCDIC machines.

    fScanIndex = fNextIndex;
    c.fChar    = nextCharLL();
    c.fEscaped = FALSE;

    //
    //  check for '' sequence.
    //  These are recognized in all contexts, whether in quoted text or not.
    //
    if (c.fChar == chApos) {
        if (fRB->fRules.char32At(fNextIndex) == chApos) {
            c.fChar    = nextCharLL();        // get nextChar officially so character counts
            c.fEscaped = TRUE;                //   stay correct.
        }
        else
        {
            // Single quote, by itself.
            //   Toggle quoting mode.
            //   Return either '('  or ')', because quotes cause a grouping of the quoted text.
            fQuoteMode = !fQuoteMode;
            if (fQuoteMode == TRUE) {
                c.fChar = chLParen;
            } else {
                c.fChar = chRParen;
            }
            c.fEscaped = FALSE;      // The paren that we return is not escaped.
            return;
        }
    }

    if (fQuoteMode) {
        c.fEscaped = TRUE;
    }
    else
    {
        // We are not in a 'quoted region' of the source.
        //
        if (c.fChar == chPound) {
            // Start of a comment.  Consume the rest of it.
            //  The new-line char that terminates the comment is always returned.
            //  It will be treated as white-space, and serves to break up anything
            //    that might otherwise incorrectly clump together with a comment in
            //    the middle (a variable name, for example.)
            for (;;) {
                c.fChar = nextCharLL();
                if (c.fChar == (UChar32)-1 ||  // EOF
                    c.fChar == chCR     ||
                    c.fChar == chLF     ||
                    c.fChar == chNEL    ||
                    c.fChar == chLS)       {break;}
            }
        }
        if (c.fChar == (UChar32)-1) {
            return;
        }

        //
        //  check for backslash escaped characters.
        //  Use UnicodeString::unescapeAt() to handle them.
        //
        if (c.fChar == chBackSlash) {
            c.fEscaped = TRUE;
            int32_t startX = fNextIndex;
            c.fChar = fRB->fRules.unescapeAt(fNextIndex);
            if (fNextIndex == startX) {
                error(U_BRK_HEX_DIGITS_EXPECTED);
            }
            fCharNum += fNextIndex-startX;
        }
    }
    // putc(c.fChar, stdout);
}

//---------------------------------------------------------------------------------
//
//  Parse RBBI rules.   The state machine for rules parsing is here.
//                      The state tables are hand-written in the file TODO.txt,
//                      and converted to the form used here by a perl
//                      script rbbicst.pl
//
//---------------------------------------------------------------------------------
void RBBIRuleScanner::parse() {
    uint16_t                state;
    const RBBIRuleTableEl  *tableEl;

    if (U_FAILURE(*fRB->fStatus)) {
        return;
    }

    state = 1;
    nextChar(fC);
    //
    // Main loop for the rule parsing state machine.
    //   Runs once per state transition.
    //   Each time through optionally performs, depending on the state table,
    //      - an advance to the the next input char
    //      - an action to be performed.
    //      - pushing or popping a state to/from the local state return stack.
    //
    for (;;) {
        //  Bail out if anything has gone wrong.
        //  RBBI rule file parsing stops on the first error encountered.
        if (U_FAILURE(*fRB->fStatus)) {
            break;
        }

        // Quit if state == 0.  This is the normal way to exit the state machine.
        //
        if (state == 0) {
            break;
        }

        // Find the state table element that matches the input char from the rule, or the
        //    class of the input character.  Start with the first table row for this
        //    state, then linearly scan forward until we find a row that matches the
        //    character.  The last row for each state always matches all characters, so
        //    the search will stop there, if not before.
        //
        tableEl = &gRuleParseStateTable[state];
        if (fRB->fDebugEnv && uprv_strstr(fRB->fDebugEnv, "scan")) {
            RBBIDebugPrintf("char, line, col = (\'%c\', %d, %d)    state=%s ",
                fC.fChar, fLineNum, fCharNum, RBBIRuleStateNames[state]);
        }

        for (;;) {
            if (fRB->fDebugEnv && uprv_strstr(fRB->fDebugEnv, "scan")) { RBBIDebugPrintf(".");}
            if (tableEl->fCharClass < 127 && tableEl->fCharClass == fC.fChar) {
                // Table row specified an individual character, not a set, and
                //   the input character matched it.
                break;
            }
            if (tableEl->fCharClass == 255) {
                // Table row specified default, match anything character class.
                break;
            }
            if (tableEl->fCharClass == 254 && fC.fEscaped)  {
                // Table row specified "escaped" and the char was escaped.
                break;
            }
            if (tableEl->fCharClass == 253 && fC.fEscaped &&
                (fC.fChar == 0x50 || fC.fChar == 0x70 ))  {
                // Table row specified "escaped P" and the char is either 'p' or 'P'.
                break;
            }
            if (tableEl->fCharClass == 252 && fC.fChar == (UChar32)-1)  {
                // Table row specified eof and we hit eof on the input.
                break;
            }

            if (tableEl->fCharClass >= 128 && tableEl->fCharClass < 240 && fC.fChar != (UChar32)-1) {
                UnicodeSet *uniset = fRuleSets[tableEl->fCharClass-128];
                if (uniset->contains(fC.fChar)) {
                    // Table row specified a character class, or set of characters,
                    //   and the current char matches it.
                    break;
                }
            }

            // No match on this row, advance to the next  row for this state,
            tableEl++;
        }
        if (fRB->fDebugEnv && uprv_strstr(fRB->fDebugEnv, "scan")) { RBBIDebugPrintf("\n");}

        //
        // We've found the row of the state table that matches the current input
        //   character from the rules string.
        // Perform any action specified  by this row in the state table.
        if (doParseActions((EParseAction)tableEl->fAction) == FALSE) {
            // Break out of the state machine loop if the
            //   the action signalled some kind of error, or
            //   the action was to exit, occurs on normal end-of-rules-input.
            break;
        }

        if (tableEl->fPushState != 0) {
            fStackPtr++;
            if (fStackPtr >= kStackSize) {
                error(U_BRK_INTERNAL_ERROR);
                RBBIDebugPrintf("RBBIRuleScanner::parse() - state stack overflow.\n");
                fStackPtr--;
            }
            fStack[fStackPtr] = tableEl->fPushState;
        }

        if (tableEl->fNextChar) {
            nextChar(fC);
        }

        // Get the next state from the table entry, or from the
        //   state stack if the next state was specified as "pop".
        if (tableEl->fNextState != 255) {
            state = tableEl->fNextState;
        } else {
            state = fStack[fStackPtr];
            fStackPtr--;
            if (fStackPtr < 0) {
                error(U_BRK_INTERNAL_ERROR);
                RBBIDebugPrintf("RBBIRuleScanner::parse() - state stack underflow.\n");
                fStackPtr++;
            }
        }

    }

    //
    // If there were NO user specified reverse rules, set up the equivalent of ".*;"
    //
    if (fRB->fReverseTree == NULL) {
        fRB->fReverseTree  = pushNewNode(RBBINode::opStar);
        RBBINode  *operand = pushNewNode(RBBINode::setRef);
        findSetFor(kAny, operand);
        fRB->fReverseTree->fLeftChild = operand;
        operand->fParent              = fRB->fReverseTree;
        fNodeStackPtr -= 2;
    }


    //
    // Parsing of the input RBBI rules is complete.
    // We now have a parse tree for the rule expressions
    // and a list of all UnicodeSets that are referenced.
    //
    if (fRB->fDebugEnv && uprv_strstr(fRB->fDebugEnv, "symbols")) {fSymbolTable->print();}
    if (fRB->fDebugEnv && uprv_strstr(fRB->fDebugEnv, "ptree"))
    {
        RBBIDebugPrintf("Completed Forward Rules Parse Tree...\n");
        fRB->fForwardTree->printTree();
        RBBIDebugPrintf("\nCompleted Reverse Rules Parse Tree...\n");
        fRB->fReverseTree->printTree();
    }

}


//---------------------------------------------------------------------------------
//
//  printNodeStack     for debugging...
//
//---------------------------------------------------------------------------------
void RBBIRuleScanner::printNodeStack(const char *title) {
    int i;
    RBBIDebugPrintf("%s.  Dumping node stack...\n", title);
    for (i=fNodeStackPtr; i>0; i--) {fNodeStack[i]->printTree();};
}




//---------------------------------------------------------------------------------
//
//  pushNewNode   create a new RBBINode of the specified type and push it
//                onto the stack of nodes.
//
//---------------------------------------------------------------------------------
RBBINode  *RBBIRuleScanner::pushNewNode(RBBINode::NodeType  t) {
    fNodeStackPtr++;
    if (fNodeStackPtr >= kStackSize) {
        error(U_BRK_INTERNAL_ERROR);
        RBBIDebugPrintf("RBBIRuleScanner::pushNewNode - stack overflow.\n");
        *fRB->fStatus = U_BRK_INTERNAL_ERROR;
        return NULL;
    }
    fNodeStack[fNodeStackPtr] = new RBBINode(t);
    if (fNodeStack[fNodeStackPtr] == NULL) {
        *fRB->fStatus = U_MEMORY_ALLOCATION_ERROR;
    }
    return fNodeStack[fNodeStackPtr];
};



//---------------------------------------------------------------------------------
//
//  scanSet    Construct a UnicodeSet from the text at the current scan
//             position.  Advance the scan position to the first character
//             after the set.
//
//             A new RBBI setref node referring to the set is pushed onto the node
//             stack.
//
//             The scan position is normally under the control of the state machine
//             that controls rule parsing.  UnicodeSets, however, are parsed by
//             the UnicodeSet constructor, not by the RBBI rule parser.
//
//---------------------------------------------------------------------------------
void RBBIRuleScanner::scanSet() {
    UnicodeSet    *uset;
    ParsePosition  pos;
    int            startPos;
    int            i;

    if (U_FAILURE(*fRB->fStatus)) {
        return;
    }

    pos.setIndex(fScanIndex);
    startPos = fScanIndex;
    UErrorCode localStatus = U_ZERO_ERROR;
    uset = new UnicodeSet(fRB->fRules, pos,
                         *fSymbolTable,
                         localStatus);
    if (U_FAILURE(localStatus)) {
        //  TODO:  Get more accurate position of the error from UnicodeSet's return info.
        //         UnicodeSet appears to not be reporting correctly at this time.
        RBBIDebugPrintf("UnicodeSet parse postion.ErrorIndex = %d\n", pos.getIndex());
         error(localStatus);
         return;
    }

    // Advance the RBBI parse postion over the UnicodeSet pattern.
    //   Don't just set fScanIndex because the line/char positions maintained
    //   for error reporting would be thrown off.
    i = pos.getIndex();
    for (;;) {
        if (fNextIndex >= i) {
            break;
        }
        nextCharLL();
    }

    if (U_SUCCESS(*fRB->fStatus)) {
        RBBINode         *n;

        n = pushNewNode(RBBINode::setRef);
        n->fFirstPos = startPos;
        n->fLastPos  = fNextIndex;
        fRB->fRules.extractBetween(n->fFirstPos, n->fLastPos, n->fText);
        //  findSetFor() serves several purposes here:
        //     - Adopts storage for the UnicodeSet, will be responsible for deleting.
        //     - Mantains collection of all sets in use, needed later for establishing
        //          character categories for run time engine.
        //     - Eliminates mulitiple instances of the same set.
        //     - Creates a new uset node if necessary (if this isn't a duplicate.)
        findSetFor(n->fText, n, uset);
    }

};


U_NAMESPACE_END


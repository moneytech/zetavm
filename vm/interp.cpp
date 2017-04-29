#include <cassert>
#include <iostream>
#include <unordered_map>
#include "runtime.h"
#include "parser.h"
#include "interp.h"
#include "core.h"

/// Inline cache to speed up property lookups
class ICache
{
private:

    // Cached slot index
    size_t slotIdx = 0;

    // Field name to look up
    std::string fieldName;

public:

    ICache(std::string fieldName)
    : fieldName(fieldName)
    {
    }

    Value getField(Object obj)
    {
        Value val;

        if (!obj.getField(fieldName.c_str(), val, slotIdx))
        {
            throw RunError("missing field \"" + fieldName + "\"");
        }

        return val;
    }

    int64_t getInt64(Object obj)
    {
        auto val = getField(obj);
        assert (val.isInt64());
        return (int64_t)val;
    }

    String getStr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isString());
        return String(val);
    }

    Object getObj(Object obj)
    {
        auto val = getField(obj);
        assert (val.isObject());
        return Object(val);
    }

    Array getArr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isArray());
        return Array(val);
    }
};

std::string posToString(Value srcPos)
{
    assert (srcPos.isObject());
    auto srcPosObj = (Object)srcPos;

    auto lineNo = (int64_t)srcPosObj.getField("line_no");
    auto colNo = (int64_t)srcPosObj.getField("col_no");
    auto srcName = (std::string)srcPosObj.getField("src_name");

    return (
        srcName + "@" +
        std::to_string(lineNo) + ":" +
        std::to_string(colNo)
    );
}

/// Opcode enumeration
enum Opcode : uint16_t
{
    GET_LOCAL,
    SET_LOCAL,

    // Stack manipulation
    PUSH,
    POP,
    DUP,
    SWAP,

    // 64-bit integer operations
    ADD_I64,
    SUB_I64,
    MUL_I64,
    LT_I64,
    LE_I64,
    GT_I64,
    GE_I64,
    EQ_I64,

    // String operations
    STR_LEN,
    GET_CHAR,
    GET_CHAR_CODE,
    STR_CAT,
    EQ_STR,

    // Object operations
    NEW_OBJECT,
    HAS_FIELD,
    SET_FIELD,
    GET_FIELD,
    EQ_OBJ,

    // Miscellaneous
    EQ_BOOL,
    HAS_TAG,
    GET_TAG,

    // Array operations
    NEW_ARRAY,
    ARRAY_LEN,
    ARRAY_PUSH,
    GET_ELEM,
    SET_ELEM,

    // Branch instructions
    // Note: opcode for stub branches is opcode+1
    JUMP,
    JUMP_STUB,
    IF_TRUE,
    IF_TRUE_STUB,
    CALL,
    RET,

    IMPORT,
    ABORT
};

/// Map from pointers to instruction objects to opcodes
std::unordered_map<refptr, Opcode> opCache;

/// Total count of instructions executed
size_t cycleCount = 0;

/// Cache of all possible one-character string values
Value charStrings[256];

Opcode decode(Object instr)
{
    auto instrPtr = (refptr)instr;

    if (opCache.find(instrPtr) != opCache.end())
    {
        //std::cout << "cache hit" << std::endl;
        return opCache[instrPtr];
    }

    // Get the opcode string for this instruction
    static ICache opIC("op");
    auto opStr = (std::string)opIC.getStr(instr);

    //std::cout << "decoding \"" << opStr << "\"" << std::endl;

    Opcode op;

    // Local variable access
    if (opStr == "get_local")
        op = GET_LOCAL;
    else if (opStr == "set_local")
        op = SET_LOCAL;

    // Stack manipulation
    else if (opStr == "push")
        op = PUSH;
    else if (opStr == "pop")
        op = POP;
    else if (opStr == "dup")
        op = DUP;
    else if (opStr == "push")
        op = PUSH;

    // 64-bit integer operations
    else if (opStr == "add_i64")
        op = ADD_I64;
    else if (opStr == "sub_i64")
        op = SUB_I64;
    else if (opStr == "mul_i64")
        op = MUL_I64;
    else if (opStr == "lt_i64")
        op = LT_I64;
    else if (opStr == "le_i64")
        op = LE_I64;
    else if (opStr == "gt_i64")
        op = GT_I64;
    else if (opStr == "ge_i64")
        op = GE_I64;
    else if (opStr == "eq_i64")
        op = EQ_I64;

    // String operations
    else if (opStr == "str_len")
        op = STR_LEN;
    else if (opStr == "get_char")
        op = GET_CHAR;
    else if (opStr == "get_char_code")
        op = GET_CHAR_CODE;
    else if (opStr == "str_cat")
        op = STR_CAT;
    else if (opStr == "eq_str")
        op = EQ_STR;

    // Object operations
    else if (opStr == "new_object")
        op = NEW_OBJECT;
    else if (opStr == "has_field")
        op = HAS_FIELD;
    else if (opStr == "set_field")
        op = SET_FIELD;
    else if (opStr == "get_field")
        op = GET_FIELD;
    else if (opStr == "eq_obj")
        op = EQ_OBJ;

    // Array operations
    else if (opStr == "new_array")
        op = NEW_ARRAY;
    else if (opStr == "array_len")
        op = ARRAY_LEN;
    else if (opStr == "array_push")
        op = ARRAY_PUSH;
    else if (opStr == "get_elem")
        op = GET_ELEM;
    else if (opStr == "set_elem")
        op = SET_ELEM;

    // Miscellaneous
    else if (opStr == "eq_bool")
        op = EQ_BOOL;
    else if (opStr == "has_tag")
        op = HAS_TAG;

    // Branch instructions
    else if (opStr == "jump")
        op = JUMP;
    else if (opStr == "if_true")
        op = IF_TRUE;
    else if (opStr == "call")
        op = CALL;
    else if (opStr == "ret")
        op = RET;

    // VM interface
    else if (opStr == "import")
        op = IMPORT;
    else if (opStr == "abort")
        op = ABORT;

    else
        throw RunError("unknown op in decode \"" + opStr + "\"");

    opCache[instrPtr] = op;
    return op;
}

Value call(Object fun, ValueVec args)
{
    static ICache numParamsIC("num_params");
    static ICache numLocalsIC("num_locals");
    auto numParams = numParamsIC.getInt64(fun);
    auto numLocals = numLocalsIC.getInt64(fun);
    assert (args.size() <= numParams);
    assert (numParams <= numLocals);

    ValueVec locals;
    locals.resize(numLocals, Value::UNDEF);

    // Copy the arguments into the locals
    for (size_t i = 0; i < args.size(); ++i)
    {
        //std::cout << "  " << args[i].toString() << std::endl;
        locals[i] = args[i];
    }

    // Temporary value stack
    ValueVec stack;

    // Array of instructions to execute
    Value instrs;

    // Number of instructions in the current block
    size_t numInstrs = 0;

    // Index of the next instruction to execute
    size_t instrIdx = 0;

    auto popVal = [&stack]()
    {
        if (stack.empty())
            throw RunError("op cannot pop value, stack empty");
        auto val = stack.back();
        stack.pop_back();
        return val;
    };

    auto popBool = [&popVal]()
    {
        auto val = popVal();
        if (!val.isBool())
            throw RunError("op expects boolean value");
        return (bool)val;
    };

    auto popInt64 = [&popVal]()
    {
        auto val = popVal();
        if (!val.isInt64())
            throw RunError("op expects int64 value");
        return (int64_t)val;
    };

    auto popStr = [&popVal]()
    {
        auto val = popVal();
        if (!val.isString())
            throw RunError("op expects string value");
        return String(val);
    };

    auto popArray = [&popVal]()
    {
        auto val = popVal();
        if (!val.isArray())
            throw RunError("op expects array value");
        return Array(val);
    };

    auto popObj = [&popVal]()
    {
        auto val = popVal();
        assert (val.isObject());
        return Object(val);
    };

    auto pushBool = [&stack](bool val)
    {
        stack.push_back(val? Value::TRUE:Value::FALSE);
    };

    auto branchTo = [&instrs, &numInstrs, &instrIdx](Object targetBB)
    {
        //std::cout << "branching" << std::endl;

        if (instrIdx != numInstrs)
        {
            throw RunError(
                "only the last instruction in a block can be a branch ("
                "instrIdx=" + std::to_string(instrIdx) + ", " +
                "numInstrs=" + std::to_string(numInstrs) + ")"
            );
        }

        static ICache instrsIC("instrs");
        Array instrArr = instrsIC.getArr(targetBB);

        instrs = (Value)instrArr;
        numInstrs = instrArr.length();
        instrIdx = 0;

        if (numInstrs == 0)
        {
            throw RunError("target basic block is empty");
        }
    };

    // Get the entry block for this function
    static ICache entryIC("entry");
    Object entryBB = entryIC.getObj(fun);

    // Branch to the entry block
    branchTo(entryBB);

    // For each instruction to execute
    for (;;)
    {
        assert (instrIdx < numInstrs);

        //std::cout << "cycleCount=" << cycleCount << std::endl;
        //std::cout << "instrIdx=" << instrIdx << std::endl;

        Array instrArr = Array(instrs);
        Value instrVal = instrArr.getElem(instrIdx);
        assert (instrVal.isObject());
        auto instr = Object(instrVal);

        cycleCount++;
        instrIdx++;

        // Get the opcode for this instruction
        auto op = decode(instr);

        switch (op)
        {
            // Read a local variable and push it on the stack
            case GET_LOCAL:
            {
                static ICache icache("idx");
                auto localIdx = icache.getInt64(instr);
                //std::cout << "localIdx=" << localIdx << std::endl;
                assert (localIdx < locals.size());
                stack.push_back(locals[localIdx]);
            }
            break;

            // Set a local variable
            case SET_LOCAL:
            {
                static ICache icache("idx");
                auto localIdx = icache.getInt64(instr);
                //std::cout << "localIdx=" << localIdx << std::endl;
                assert (localIdx < locals.size());
                locals[localIdx] = popVal();
            }
            break;

            case PUSH:
            {
                static ICache icache("val");
                auto val = icache.getField(instr);
                stack.push_back(val);
            }
            break;

            case POP:
            {
                if (stack.empty())
                    throw RunError("pop failed, stack empty");
                stack.pop_back();
            }
            break;

            // Duplicate the top of the stack
            case DUP:
            {
                static ICache idxIC("idx");
                auto idx = idxIC.getInt64(instr);

                if (idx >= stack.size())
                    throw RunError("stack undeflow, invalid index for dup");

                auto val = stack[stack.size() - 1 - idx];
                stack.push_back(val);
            }
            break;

            // Swap the topmost two stack elements
            case SWAP:
            {
                auto v0 = popVal();
                auto v1 = popVal();
                stack.push_back(v0);
                stack.push_back(v1);
            }
            break;

            //
            // 64-bit integer operations
            //

            case ADD_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 + arg1);
            }
            break;

            case SUB_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 - arg1);
            }
            break;

            case MUL_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                stack.push_back(arg0 * arg1);
            }
            break;

            case LT_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 < arg1);
            }
            break;

            case LE_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 <= arg1);
            }
            break;

            case GT_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 > arg1);
            }
            break;

            case GE_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 >= arg1);
            }
            break;

            case EQ_I64:
            {
                auto arg1 = popInt64();
                auto arg0 = popInt64();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // String operations
            //

            case STR_LEN:
            {
                auto str = popStr();
                stack.push_back(str.length());
            }
            break;

            case GET_CHAR:
            {
                auto idx = (size_t)popInt64();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char, index out of bounds"
                    );
                }

                auto ch = str[idx];

                // Cache single-character strings
                if (charStrings[ch] == Value::FALSE)
                {
                    char buf[2] = { (char)str[idx], '\0' };
                    charStrings[ch] = String(buf);
                }

                stack.push_back(charStrings[ch]);
            }
            break;

            case GET_CHAR_CODE:
            {
                auto idx = (size_t)popInt64();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char_code, index out of bounds"
                    );
                }

                stack.push_back((int64_t)str[idx]);
            }
            break;

            case STR_CAT:
            {
                auto a = popStr();
                auto b = popStr();
                auto c = String::concat(b, a);
                stack.push_back(c);
            }
            break;

            case EQ_STR:
            {
                auto arg1 = popStr();
                auto arg0 = popStr();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // Object operations
            //

            case NEW_OBJECT:
            {
                auto capacity = popInt64();
                auto obj = Object::newObject(capacity);
                stack.push_back(obj);
            }
            break;

            case HAS_FIELD:
            {
                auto fieldName = popStr();
                auto obj = popObj();
                pushBool(obj.hasField(fieldName));
            }
            break;

            case SET_FIELD:
            {
                auto val = popVal();
                auto fieldName = popStr();
                auto obj = popObj();

                if (!isValidIdent(fieldName))
                {
                    throw RunError(
                        "invalid identifier in set_field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                obj.setField(fieldName, val);
            }
            break;

            // This instruction will abort execution if trying to
            // access a field that is not present on an object.
            // The running program is responsible for testing that
            // fields exist before attempting to read them.
            case GET_FIELD:
            {
                auto fieldName = popStr();
                auto obj = popObj();

                //std::cout << "get " << std::string(fieldName) << std::endl;

                if (!obj.hasField(fieldName))
                {
                    throw RunError(
                        "get_field failed, missing field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                auto val = obj.getField(fieldName);
                stack.push_back(val);
            }
            break;

            case EQ_OBJ:
            {
                Value arg1 = popVal();
                Value arg0 = popVal();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // Array operations
            //

            case NEW_ARRAY:
            {
                auto len = popInt64();
                auto array = Array(len);
                stack.push_back(array);
            }
            break;

            case ARRAY_LEN:
            {
                auto arr = Array(popVal());
                stack.push_back(arr.length());
            }
            break;

            case ARRAY_PUSH:
            {
                auto val = popVal();
                auto arr = Array(popVal());
                arr.push(val);
            }
            break;

            case SET_ELEM:
            {
                auto val = popVal();
                auto idx = (size_t)popInt64();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "set_elem, index out of bounds"
                    );
                }

                arr.setElem(idx, val);
            }
            break;

            case GET_ELEM:
            {
                auto idx = (size_t)popInt64();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "get_elem, index out of bounds"
                    );
                }

                stack.push_back(arr.getElem(idx));
            }
            break;

            case EQ_BOOL:
            {
                auto arg1 = popBool();
                auto arg0 = popBool();
                pushBool(arg0 == arg1);
            }
            break;

            // Test if a value has a given tag
            case HAS_TAG:
            {
                auto tag = popVal().getTag();
                static ICache tagIC("tag");
                auto tagStr = tagIC.getStr(instr);

                switch (tag)
                {
                    case TAG_UNDEF:
                    pushBool(tagStr == "undef");
                    break;

                    case TAG_BOOL:
                    pushBool(tagStr == "bool");
                    break;

                    case TAG_INT64:
                    pushBool(tagStr == "int64");
                    break;

                    case TAG_STRING:
                    pushBool(tagStr == "string");
                    break;

                    case TAG_ARRAY:
                    pushBool(tagStr == "array");
                    break;

                    case TAG_OBJECT:
                    pushBool(tagStr == "object");
                    break;

                    default:
                    throw RunError(
                        "unknown value type in has_tag"
                    );
                }
            }
            break;

            case JUMP:
            {
                static ICache icache("to");
                auto target = icache.getObj(instr);
                branchTo(target);
            }
            break;

            case IF_TRUE:
            {
                static ICache thenCache("then");
                static ICache elseCache("else");
                auto thenBB = thenCache.getObj(instr);
                auto elseBB = elseCache.getObj(instr);
                auto arg0 = popVal();
                branchTo((arg0 == Value::TRUE)? thenBB:elseBB);
            }
            break;

            // Regular function call
            case CALL:
            {
                static ICache retToCache("ret_to");
                static ICache numArgsCache("num_args");
                auto retToBB = retToCache.getObj(instr);
                auto numArgs = numArgsCache.getInt64(instr);

                auto callee = popVal();

                if (stack.size() < numArgs)
                {
                    throw RunError(
                        "stack underflow at call"
                    );
                }

                // Copy the arguments into a vector
                ValueVec args;
                args.resize(numArgs);
                for (size_t i = 0; i < numArgs; ++i)
                    args[numArgs - 1 - i] = popVal();

                static ICache numParamsIC("num_params");
                size_t numParams;
                if (callee.isObject())
                {
                    numParams = numParamsIC.getInt64(callee);
                }
                else if (callee.isHostFn())
                {
                    auto hostFn = (HostFn*)(callee.getWord().ptr);
                    numParams = hostFn->getNumParams();
                }
                else
                {
                    throw RunError("invalid callee at call site");
                }

                if (numArgs != numParams)
                {
                    std::string srcPosStr = (
                        instr.hasField("src_pos")?
                        (posToString(instr.getField("src_pos")) + " - "):
                        std::string("")
                    );

                    throw RunError(
                        srcPosStr +
                        "incorrect argument count in call, received " +
                        std::to_string(numArgs) +
                        ", expected " +
                        std::to_string(numParams)
                    );
                }

                Value retVal;

                if (callee.isObject())
                {
                    // Perform the call
                    retVal = call(callee, args);
                }
                else if (callee.isHostFn())
                {
                    auto hostFn = (HostFn*)(callee.getWord().ptr);

                    // Call the host function
                    switch (numArgs)
                    {
                        case 0:
                        retVal = hostFn->call0();
                        break;

                        case 1:
                        retVal = hostFn->call1(args[0]);
                        break;

                        case 2:
                        retVal = hostFn->call2(args[0], args[1]);
                        break;

                        case 3:
                        retVal = hostFn->call3(args[0], args[1], args[2]);
                        break;

                        default:
                        assert (false);
                    }
                }

                // Push the return value on the stack
                stack.push_back(retVal);

                // Jump to the return basic block
                branchTo(retToBB);
            }
            break;

            case RET:
            {
                auto val = stack.back();
                stack.pop_back();
                return val;
            }
            break;

            case IMPORT:
            {
                auto pkgName = popStr();
                auto pkg = import(pkgName);
                stack.push_back(pkg);
            }
            break;

            case ABORT:
            {
                auto errMsg = (std::string)popStr();

                // If a source position was specified
                if (instr.hasField("src_pos"))
                {
                    auto srcPos = instr.getField("src_pos");
                    std::cout << posToString(srcPos) << " - ";
                }

                if (errMsg != "")
                {
                    std::cout << "aborting execution due to error: ";
                    std::cout << errMsg << std::endl;
                }
                else
                {
                    std::cout << "aborting execution due to error" << std::endl;
                }

                exit(-1);
            }
            break;

            default:
            assert (false && "unhandled op in interpreter");
        }
    }

    assert (false);
}

/// Call a function exported by a package
Value callExportFn(
    Object pkg,
    std::string fnName,
    ValueVec args
)
{
    assert (pkg.hasField(fnName));
    auto fnVal = pkg.getField(fnName);
    assert (fnVal.isObject());
    auto funObj = Object(fnVal);

    return call(funObj, args);
}

Value testRunImage(std::string fileName)
{
    std::cout << "loading image \"" << fileName << "\"" << std::endl;

    auto pkg = parseFile(fileName);

    return callExportFn(pkg, "main");
}

void testInterp()
{
    std::cout << "interpreter tests" << std::endl;

    assert (testRunImage("tests/zetavm/ex_ret_cst.zim") == Value(777));
    assert (testRunImage("tests/zetavm/ex_loop_cnt.zim") == Value(0));
    assert (testRunImage("tests/zetavm/ex_image.zim") == Value(10));
    assert (testRunImage("tests/zetavm/ex_rec_fact.zim") == Value(5040));
    assert (testRunImage("tests/zetavm/ex_fibonacci.zim") == Value(377));
}

//============================================================================
// New interpreter
//============================================================================

class CodeFragment
{
public:

    /// Start index in the executable heap
    //uint32_t startIdx = uint32_t.max;

    /// End index in the executable heap
    //uint32_t endIdx = uint32_t.max;

    /*
    /// Get the length of the code fragment
    final auto length()
    {
        assert (startIdx !is startIdx.max);
        assert (ended);
        return endIdx - startIdx;
    }
    */

    /*
    /// Store the start position of the code
    final void markStart(CodeBlock as)
    {
        assert (
            startIdx is startIdx.max,
            "start position is already marked"
        );

        startIdx = cast(uint32_t)as.getWritePos();

        // Add a label string comment
        if (opts.genasm)
            as.writeString(this.getName ~ ":");
    }
    */

    /*
    /// Store the end position of the code
    final void markEnd(CodeBlock as)
    {
        assert (
            !ended,
            "end position is already marked"
        );

        endIdx = cast(uint32_t)as.getWritePos();

        // Add this fragment to the back of to the list of compiled fragments
        vm.fragList.assumeSafeAppend ~= this;

        // Update the generated code size stat
        stats.genCodeSize += this.length();
    }
    */

    /*
    /// Check if the fragment start has been marked (fragment is instantiated)
    final bool started()
    {
        return startIdx !is startIdx.max;
    }

    /// Check if the end of the fragment has been marked
    final bool ended()
    {
        return endIdx !is endIdx.max;
    }
    */
};

class BlockVersion : CodeFragment
{
public:

    /// Associated block
    Object block;

    /// Code generation context at block entry
    //CodeGenCtx ctx;

    /// Branch targets
    //CodeFragment[] targets;

    BlockVersion(Object block)
    : block(block)
    {
    }

    /*
    /// Get a pointer to the executable code for this version
    final auto getCodePtr(CodeBlock cb)
    {
        return cb.getAddress(startIdx);
    }
    */
};

typedef std::vector<BlockVersion*> VersionList;

/// Flat array of bytes into which code gets compiled
uint8_t* codeHeap = nullptr;

/// Limit pointer for the code heap
uint8_t* codeHeapLimit = nullptr;

/// Current allocation pointer in the code heap
uint8_t* codeHeapAlloc = nullptr;

/// Map of block objects to lists of versions
std::unordered_map<refptr, VersionList> versionMap;

/// Ordered list of allocated code fragments
std::vector<CodeFragment*> fragmentList;

/// Lower stack limit (stack pointer must be greater than this)
Word* stackLimit = nullptr;

/// Stack bottom (end of the stack memory array)
Word* stackBottom = nullptr;

/// Stack frame base pointer
Word* basePtr = nullptr;

/// Current temp stack top pointer
Word* stackPtr = nullptr;

// Current instruction pointer
uint8_t* instrPtr = nullptr;

// TODO: do we already need a versioning context?
// we need to manage the temp stack size?
// can technically just use the stack top for this?
BlockVersion* getBlockVersion(Object Block)
{
    // TODO: version map lookup

    // Stubs vs not? How does that work?



    assert (false);
}

void compileBlock(Object block)
{
    // Block name icache, for debugging
    static ICache nameIC("name");

    // Get the instructions array
    static ICache instrsIC("instrs");
    Array instrs = instrsIC.getArr(block);

    // For each instruction
    for (size_t i = 0; i < instrs.length(); ++i)
    {


    }


    // TODO: decode instructions



    assert (false);
}

/// Start/continue execution beginning at a current instruction
void execCode(uint8_t* instrPtr)
{
    // TODO: main interpreter loop
}

/// Begin the execution of a function
Value callFun(Object fun, ValueVec args)
{
    // TODO: push args on the stack





    // TODO
    return Value(777);
}

/// Call a function exported by a package
Value callExportFnNew(
    Object pkg,
    std::string fnName,
    ValueVec args = ValueVec()
)
{
    assert (pkg.hasField(fnName));
    auto fnVal = pkg.getField(fnName);
    assert (fnVal.isObject());
    auto funObj = Object(fnVal);

    return callFun(funObj, args);
}

Value testRunImageNew(std::string fileName)
{
    std::cout << "loading image \"" << fileName << "\"" << std::endl;

    auto pkg = parseFile(fileName);

    return callExportFnNew(pkg, "main");
}

void testInterpNew()
{
    // TODO: call main function of simple test

    assert (testRunImageNew("tests/zetavm/ex_ret_cst.zim") == Value(777));
    //assert (testRunImageNew("tests/zetavm/ex_loop_cnt.zim") == Value(0));
    //assert (testRunImageNew("tests/zetavm/ex_image.zim") == Value(10));
    //assert (testRunImageNew("tests/zetavm/ex_rec_fact.zim") == Value(5040));
    //assert (testRunImageNew("tests/zetavm/ex_fibonacci.zim") == Value(377));
}

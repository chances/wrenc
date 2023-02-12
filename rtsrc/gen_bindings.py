import re
import sys
from optparse import OptionParser
from typing import TextIO, List, Optional
from dataclasses import dataclass

METHOD_REGEX = re.compile(
    r"^\s*WREN_METHOD\((?P<type>[^)]*)\)\s+(?P<stat_virt>static\s+|virtual\s+)?" +
    r"(?P<return>[\w*:]+\s+\**)(?P<name>\w+)\s*\((?P<args>.*)\)(?:\s+const)?\s*;$")
CLASS_REGEX = re.compile(r"^class\s+(?P<name>Obj\w*)\s+[:{]")
ARG_REGEX = re.compile(r"^\s*(?P<type>.+\s[*&]*)(?P<name>\w+)\s*$")
ARG_INFO_REGEX = re.compile(r'^\s*ARG\("(?P<error_name>[^"]+)"\)')

# Making a C++ method called (for example) OperatorPlus would have a Wren signature of '+'
OPERATOR_PREFIX = "Operator"
OPERATOR_TYPES = {
    "Plus": "+",
    "Minus": "-",
    "Multiply": "*",
    "Divide": "/",
    "Modulo": "%",
    "And": "&",
    "Or": "|",
    "XOr": "^",
    "LeftShift": "<<",
    "RightShift": ">>",

    "EqualTo": "==",
    "NotEqual": "!=",
    "LessThan": "<",
    "LessThanEq": "<=",
    "GreaterThan": ">",
    "GreaterThanEq": ">=",

    "BoolNegate": "!",
    "BitwiseNegate": "~",

    "DotDot": "..",
    "DotDotDot": "...",
}
OPERATOR_SUBSCRIPT = "OperatorSubscript"
OPERATOR_SUBSCRIPT_SET = "OperatorSubscriptSet"
NUMBER_CLASS = "ObjNumClass"
NULL_CLASS = "ObjNull"


@dataclass
class GenOptions:
    files: List[str]
    pre_entry_gc: bool


@dataclass
class Arg:
    type: str
    name: str
    pos: int
    error_name: Optional[str]

    def raw_name(self) -> str:
        return f"arg{self.pos}"

    def typed_name(self) -> str:
        return f"typedArg{self.pos}"


@dataclass
class Method:
    return_type: str
    name: str
    args: List[Arg]
    static: bool
    special_type: str
    parent_class: 'Class'

    def arity(self) -> int:
        count = len(self.args)

        # Num is special, since for non-static methods the first argument is the receiver
        if self.parent_class.name == NUMBER_CLASS and not self.static:
            count -= 1
            if count < 0:
                raise Exception(f"Missing number class receiver for {self.name}")

        return count

    def signature(self) -> str:
        arg_part = ",".join(["_"] * self.arity())
        name = self.name

        # Subscripts are special
        if self.name == OPERATOR_SUBSCRIPT or self.name == OPERATOR_SUBSCRIPT_SET:
            setter = self.name == OPERATOR_SUBSCRIPT_SET

            # Getters take the form [_,_,_] and setters take the form [_,_,_]=(_) - thus calculate
            # the number of arguments in the square brackets.
            num_args = self.arity()
            if setter:
                num_args -= 1
            index_part = ",".join(["_"] * num_args)

            if setter:
                return f"[{index_part}]=(_)"
            else:
                return f"[{index_part}]"

        # If this is an operator function, handle that appropriately
        if self.name.startswith(OPERATOR_PREFIX):
            op_type = self.name.removeprefix(OPERATOR_PREFIX)
            if op_type not in OPERATOR_TYPES:
                raise Exception(f"Invalid operator name {self.parent_class.name}::{self.name}")
            name = OPERATOR_TYPES[op_type]

        # Make the first letter lowercase so we can match up the C++ and Wren styling
        name = name[0].lower() + name[1:]

        if self.special_type == 'getter':
            return name

        return f"{name}({arg_part})"

    def method_name(self):
        return f"binding_{self.parent_class.name}_{self.name}_{self.arity()}"


@dataclass
class Class:
    name: str
    methods: List[Method]


def parse_file(fi: TextIO) -> List[Class]:
    current_class = None

    classes: List[Class] = []

    for line in fi:
        line = line.split("//")[0]  # Remove comments
        line = line.strip()
        if not line:
            continue

        # Keep track of the class we're in
        class_decl = CLASS_REGEX.match(line)
        if class_decl:
            current_class = Class(class_decl.group("name"), [])
            classes.append(current_class)
            continue

        # Parse the line as a method
        method_match = METHOD_REGEX.match(line)
        if not method_match:
            continue

        return_type = method_match.group("return").replace(' ', '')  # May have spaces from pointers
        name = method_match.group("name")
        args_str = method_match.group("args")
        static_virtual = method_match.group("stat_virt")
        special_type = method_match.group("type")

        # We can either have a static or a virtual qualifier - we only care about static qualifiers, and this group
        # also contains some whitespace.
        if static_virtual:
            static_virtual = static_virtual.strip()
        static = static_virtual == 'static'

        if special_type not in ['', 'getter', 'variadic']:
            raise Exception(f"Invalid special type '{special_type}' for method {current_class.name}::{name}")

        args = []
        for arg_str in args_str.split(","):
            # If there are no arguments, don't try and parse the empty string
            if args_str == "":
                continue

            error_name = None

            # Grab the argument information
            info = ARG_INFO_REGEX.match(arg_str)
            if info:
                # Get rid of this argument group from the arg, so we
                # can process it as usual.
                arg_str = arg_str.removeprefix(info.group(0)).strip()

                error_name = info.group("error_name")

            # We can't just split the argument to type+name by split(' '), since that wouldn't handle
            # pointers like "Obj *thing", as it would think the name is "*thing".
            arg_match = ARG_REGEX.match(arg_str)
            if not arg_match:
                raise Exception(f"Could not match arguments for method {current_class.name}::{name}: '{arg_str}'")

            arg_type = arg_match.group("type").replace(" ", "")  # Remove space before pointer
            args.append(Arg(arg_type, arg_match.group("name"), len(args), error_name))

        # Variadic functions have a last argument that's the array of values, and that's not something Wren
        # actually passes values into.
        if special_type == 'variadic':
            args.pop()

        method = Method(return_type, name, args, static, special_type, current_class)
        current_class.methods.append(method)

    return classes


def generate(output: TextIO, options: GenOptions):
    # Parse the headers
    classes: List[Class] = []
    for filename in options.files:
        with open(filename, "r") as fi:
            classes += parse_file(fi)

    output.write("// Auto-generated file (from gen_bindings.py), DO NOT EDIT MANUALLY\n")
    output.write("#define BINDINGS_GEN\n")
    output.write("#include \"binding_utils.h\"\n")
    output.write("#include \"Errors.h\"\n")
    output.write("#include \"WrenRuntime.h\"\n")

    output.write("\n// Includes for referenced files:\n")
    for filename in options.files:
        # If a C++ file is specified, use its header
        if filename.endswith(".cpp"):
            filename = filename[:-3] + "h"
        output.write(f"#include \"{filename}\"\n")

    for cls in classes:
        output.write(f"\n// For class {cls.name}:\n")
        for method in cls.methods:
            # The number class is special, because the receiver is the first number argument
            receiver_def = "Value receiver"
            is_num_class = cls.name == NUMBER_CLASS
            if is_num_class and not method.static:
                receiver_def = ""

            args_str = ", ".join([f"Value {arg.raw_name()}" for arg in method.args])
            if args_str and receiver_def:
                args_str = ", " + args_str  # Add a comma to go the receiver

            if method.special_type == 'variadic':
                args_str += ", const std::initializer_list<Value> &var_args"

            debug_sig = cls.name + "." + method.signature()
            output.write(f"static Value {method.method_name()}({receiver_def}{args_str}) {'{'}\n")

            # Run the GC, if that debugging option is enabled
            if options.pre_entry_gc:
                output.write("\tWrenRuntime::Instance().RunGC();\n")

            # Convert the receiver (if not static or the null or number class)
            if cls.name == NULL_CLASS:
                output.write(f"\t{cls.name} *obj = nullptr;\n")
            elif not method.static and not is_num_class:
                output.write(f"\t{cls.name} *obj = checkReceiver<{cls.name}>(\"{debug_sig}\", receiver);\n")

            for i, arg in enumerate(method.args):
                error_name = arg.error_name
                if arg.error_name is None:
                    # For single-word stuff, setting the argument name is often
                    # enough and saves lots of ARG annotations.
                    error_name = arg.name.capitalize()

                cast_expr: str
                if arg.type == "Value":
                    cast_expr = arg.raw_name()
                elif arg.type.startswith("Obj"):
                    # Remove the pointer to use as a template parameter. Yeah, this will change Obj** to Obj, but we
                    # don't support that anyway, so no big loss there.
                    non_ptr = arg.type.replace('*', '')
                    cast_expr = f"checkArg<{non_ptr}>(\"{debug_sig}\", \"{error_name}\", {i + 1}, {arg.raw_name()}, true)"
                elif arg.type == "std::string":
                    cast_expr = f"checkString(\"{debug_sig}\", \"{error_name}\", {i + 1}, {arg.raw_name()})"
                elif arg.type == "double":
                    cast_expr = f"checkDouble(\"{debug_sig}\", \"{error_name}\", {i + 1}, {arg.raw_name()})"
                elif arg.type == "int":
                    cast_expr = f"checkInt(\"{debug_sig}\", \"{error_name}\", {i + 1}, {arg.raw_name()})"
                else:
                    raise Exception(f"Unknown arg type '{arg.type}' for method '{debug_sig}'")

                output.write(f"\t{arg.type} {arg.typed_name()} = {cast_expr};\n")

            output.write("\t")

            # Capture the return value
            if method.return_type != "void":
                output.write(f"{method.return_type} ret = ")

            # Generate the method call
            typed_arg_list = [arg.typed_name() for arg in method.args]

            if method.special_type == 'variadic':
                # Pass all the vararg values through - no typechecking on these.
                typed_arg_list.append("var_args")

            typed_args = ",".join(typed_arg_list)
            if method.static:
                output.write(f"{cls.name}::{method.name}({typed_args});\n")
            elif is_num_class:
                output.write(f"{cls.name}::Instance()->{method.name}({typed_args});\n")
            else:
                output.write(f"obj->{method.name}({typed_args});\n")

            # Convert the return value, or return null
            return_value: str
            if method.return_type == "void":
                return_value = "NULL_VAL"
            elif method.return_type == "Value":
                return_value = "ret"
            elif method.return_type == "std::string":
                return_value = "encode_object(ObjString::New(ret))"
            elif method.return_type == "bool":
                return_value = "encode_object(ObjBool::Get(ret))"
            elif method.return_type == "int" or method.return_type == "double":
                return_value = "encode_number(ret)"
            elif method.return_type.startswith("Obj"):
                return_value = "encode_object(ret)"
            else:
                raise Exception(f"Invalid return type {method.return_type} for method {debug_sig}")

            output.write(f"\treturn {return_value};\n")
            output.write("}\n")

            # Variadic methods need a bunch of helper methods with different numbers of arguments. While it should be
            # fine ABI-wise to take a bunch of parameters we don't use and ignore some of them to only use a single
            # function, then we wouldn't know how many arguments were passed. Thus we need one function for each
            # number of arguments we can take. Wren picks 17 (16+no args) as the number you can pass to Fn.call
            # so we'll copy that.
            if method.special_type == 'variadic':
                for num in range(17):
                    args = "Value r" + "".join([f",Value a{n}" for n in range(num)])
                    forward_args = ",".join([f"a{n}" for n in range(num)])

                    body = f"return {method.method_name()}(r, {'{'} {forward_args} {'}'});"

                    output.write(f"static Value {method.method_name()}_va{num}({args}) {'{'} {body} {'}'}\n")

        output.write(f"static void register_{cls.name}(ObjClass *cls, bool isMeta) {'{'}\n")
        for method in cls.methods:
            if method.static:
                meta_requirement = "isMeta"
            else:
                meta_requirement = "!isMeta"
            output.write(f"\tif ({meta_requirement})\n")
            if not method.special_type == 'variadic':
                output.write(
                    f"\t\tcls->AddFunction(\"{method.signature()}\", (void*){method.method_name()});\n")
                continue

            # Write out all the variadic functions
            output.write("\t{\n")
            for num in range(17):
                signature = method.signature()[:-1]  # Cut off the last bracket

                # Add the new arguments
                if num != 0 and signature[-1] == '_':
                    signature += ","
                signature += ",".join(["_"] * num)
                signature += ")"  # Put the bracket back on

                output.write(
                    f"\t\tcls->AddFunction(\"{signature}\", (void*){method.method_name()}_va{num});\n")
            output.write("\t}\n")

        output.write("}\n")

    output.write("\n// Binding setup method, called by hand-written C++ classes\n")
    output.write("void ObjClass::Bind(ObjClass *cls, const std::string &type, bool isMeta) {\n")
    for cls in classes:
        output.write(f"\tif (type == \"{cls.name}\") {'{'}\n")
        output.write(f"\t\tregister_{cls.name}(cls, isMeta);\n")
        output.write(f"\t\treturn;\n")
        output.write("\t}\n")
    output.write("\terrors::wrenAbort(\"Unknown bindings class '%s'\", type.c_str());\n")
    output.write("}\n")

    # Write the ObjFn dispatch code, depending on how many arguments we need
    # It doesn't really belong here, but since this is the only autogenerated file it's convenient to toss it in
    output.write("\n// ObjFn variable-argument dispatch\n")
    max_args = 16 + 1  # same as Wren, +1 to make it inclusive
    for i in range(max_args):
        arg_names = [f"v{n}" for n in range(i)]
        args_decl = "".join([f", Value {name}" for name in arg_names])
        arg_vals = ", ".join(arg_names)
        args_sig = ",Value" * i

        arg_vals_comma = arg_vals
        if i != 0:
            arg_vals_comma = ", " + arg_vals

        output.write(f"static Value fnDispatch{i}(void *fn, void *upvalues{args_decl}) {'{'}\n")
        output.write(f"\ttypedef Value (*withUpvalue)(void *upvalues{args_sig});\n")
        output.write(f"\ttypedef Value (*withoutUpvalue)({args_sig.lstrip(',')});\n")
        output.write("\tif (upvalues) {\n")
        output.write(f"\t\treturn ((withUpvalue)fn)(upvalues{arg_vals_comma});\n")
        output.write("\t}\n")
        output.write(f"\treturn ((withoutUpvalue)fn)({arg_vals});\n")
        output.write("}\n")

    output.write(
        "Value ObjFn::FunctionDispatch(void *fn, void *upvalues, int arity, const std::initializer_list<Value> &args) {\n")
    output.write("\tconst Value *a = args.begin();\n")
    for i in range(max_args):
        split_args = "".join([f", a[{n}]" for n in range(i)])
        output.write(f"\tif (arity == {i})\n")
        output.write(f"\t\treturn fnDispatch{i}(fn, upvalues {split_args});\n")
    output.write("\terrors::wrenAbort(\"Unsupported function arity: %d\", (int)args.size());\n")
    output.write("}\n")


def main():
    parser = OptionParser()
    parser.add_option("--output", dest="filename", help="The filename to write to, or stdout if omitted",
                      metavar="FILE")
    parser.add_option("--pre-entry-gc", dest="pre_entry_gc", action="store_true",
                      help="Run the GC before every function call, for testing")

    (options, args) = parser.parse_args()

    gen_opts = GenOptions(args, options.pre_entry_gc)

    if options.filename:
        with open(options.filename, "w") as fi:
            generate(fi, gen_opts)
    else:
        generate(sys.stdout, gen_opts)


if __name__ == "__main__":
    main()

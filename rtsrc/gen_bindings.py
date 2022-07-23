import io
import re
import sys
from optparse import OptionParser
from typing import TextIO, List
from dataclasses import dataclass

METHOD_REGEX = re.compile(
    r"^\s*WREN_METHOD\(\)\s+(?P<static>static\s+)?(?P<return>[\w*:]+\s+\**)(?P<name>\w+)\s*\((?P<args>[^)]*)\)\s*;$")
CLASS_REGEX = re.compile(r"^class\s+(?P<name>Obj\w+)\s+:")

# Making a C++ method called (for example) OperatorPlus would have a Wren signature of '+'
OPERATOR_PREFIX = "Operator"
OPERATOR_TYPES = {
    "Plus": "+",
    "Minus": "-",
    "Multiply": "*",
    "Divide": "/",

    "Equals": "==",
}
NUMBER_CLASS = "ObjNumClass"


@dataclass
class Arg:
    type: str
    name: str
    pos: int

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
    parent_class: 'Class'

    def arity(self) -> int:
        count = len(self.args)

        # Num is special, since for non-static methods the first argument is the receiver
        if self.parent_class.name == NUMBER_CLASS and not self.static:
            count -= 1
            if count < 1:
                raise Exception(f"Missing number class receiver for {self.name}")

        return count

    def signature(self) -> str:
        arg_part = ",".join(["_"] * self.arity())
        name = self.name

        # If this is an operator function, handle that appropriately
        if self.name.startswith(OPERATOR_PREFIX):
            op_type = self.name.removeprefix(OPERATOR_PREFIX)
            if op_type not in OPERATOR_TYPES:
                raise Exception(f"Invalid operator name {self.name}")
            name = OPERATOR_TYPES[op_type]

        # Make the first letter lowercase so we can match up the C++ and Wren styling
        name = name[0].lower() + name[1:]
        return f"{name}({arg_part})"

    def method_name(self, cls: "Class"):
        return f"binding_{cls.name}_{self.name}_{self.arity()}"


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
        static = method_match.group("static") is not None

        args = []
        for arg_str in args_str.split(","):
            # If there are no arguments, don't try and parse the empty string
            if args_str == "":
                continue

            arg_str = arg_str.strip()
            parts = arg_str.split(" ")
            assert len(parts) == 2
            args.append(Arg(parts[0], parts[1], len(args)))

        method = Method(return_type, name, args, static, current_class)
        current_class.methods.append(method)

    return classes


def generate(output: TextIO, files: List[str]):
    # Parse the headers
    classes: List[Class] = []
    for filename in files:
        with open(filename, "r") as fi:
            classes += parse_file(fi)

    output.write("// Auto-generated file (from gen_bindings.py), DO NOT EDIT MANUALLY\n")
    output.write("#include \"binding_utils.h\"\n")

    output.write("\n// Includes for referenced files:\n")
    for filename in files:
        # If a C++ file is specified, use it's header
        if filename.endswith(".cpp"):
            filename = filename[:-3] + "h"
        output.write(f"#include \"{filename}\"\n")

    for cls in classes:
        output.write(f"\n// For class {cls.name}:\n")
        for method in cls.methods:
            # The number class is special, because the receiver is the first number argument
            receiver_def = "Value receiver"
            is_num_class = cls.name == NUMBER_CLASS
            if is_num_class:
                receiver_def = ""

            args_str = ", ".join([f"Value {arg.raw_name()}" for arg in method.args])
            if args_str and receiver_def:
                args_str = ", " + args_str  # Add a comma to go the receiver
            debug_sig = cls.name + "." + method.signature()
            output.write(f"static Value {method.method_name(cls)}({receiver_def}{args_str}) {'{'}\n")

            # Convert the receiver (if not static or the number class)
            if not method.static and not is_num_class:
                output.write(f"\t{cls.name} *obj = checkReceiver<{cls.name}>(\"{debug_sig}\", receiver);\n")

            for i, arg in enumerate(method.args):
                cast_expr: str
                if arg.type == "Value":
                    cast_expr = arg.raw_name()
                elif arg.type.startswith("Obj"):
                    cast_expr = f"checkArg<{arg.type}>(\"{debug_sig}\", {i + 1}, {arg.raw_name()})"
                elif arg.type == "std::string":
                    cast_expr = f"checkString(\"{debug_sig}\", {i + 1}, {arg.raw_name()})"
                elif arg.type == "double":
                    cast_expr = f"checkDouble(\"{debug_sig}\", {i + 1}, {arg.raw_name()})"
                elif arg.type == "int":
                    cast_expr = f"checkInt(\"{debug_sig}\", {i + 1}, {arg.raw_name()})"
                else:
                    raise Exception(f"Unknown arg type '{arg.type}' for method '{debug_sig}'")

                output.write(f"\t{arg.type} {arg.typed_name()} = {cast_expr};\n")

            output.write("\t")

            # Capture the return value
            if method.return_type != "void":
                output.write(f"{method.return_type} ret = ")

            # Generate the method call
            typed_args = ",".join([arg.typed_name() for arg in method.args])
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

        output.write(f"static void register_{cls.name}(ObjClass *cls, bool isMeta) {'{'}\n")
        for method in cls.methods:
            if method.static:
                meta_requirement = "isMeta"
            else:
                meta_requirement = "!isMeta"
            output.write(f"\tif ({meta_requirement})\n")
            output.write(f"\t\tcls->AddFunction(\"{method.signature()}\", (void*){method.method_name(cls)});\n")
        output.write("}\n")

    output.write("\n// Binding setup method, called by hand-written C++ classes\n")
    output.write("void ObjNativeClass::Bind(ObjClass *cls, const std::string &type, bool isMeta) {\n")
    for cls in classes:
        output.write(f"\tif (type == \"{cls.name}\") {'{'}\n")
        output.write(f"\t\tregister_{cls.name}(cls, isMeta);\n")
        output.write(f"\t\treturn;\n")
        output.write("\t}\n")
    output.write("\tfprintf(stderr, \"Unknown bindings class '%s'\\n\", type.c_str());\n")
    output.write("\tabort();\n")
    output.write("}\n")


def main():
    parser = OptionParser()
    parser.add_option("--output", dest="filename", help="The filename to write to, or stdout if omitted",
                      metavar="FILE")

    (options, args) = parser.parse_args()

    if options.filename:
        with open(options.filename, "w") as fi:
            generate(fi, args)
    else:
        generate(sys.stdout, args)


if __name__ == "__main__":
    main()

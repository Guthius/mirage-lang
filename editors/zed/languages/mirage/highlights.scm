; Literals

(comment) @comment

(string_literal) @string
(char_literal) @string
(escape_sequence) @string.escape

(integer_literal) @number
(float_literal) @number

(boolean_literal) @constant.builtin.boolean
(nil) @constant.builtin
(iota) @constant.builtin
(default_expression) @constant.builtin
(undefined_expression) @constant.builtin

; Declarations

(function_declaration name: (identifier) @function)
(extern_function_declaration name: (identifier) @function)
(method_declaration name: (identifier) @function.method)
(trait_method_declaration name: (identifier) @function.method)
(macro_declaration name: (identifier) @function.macro)

(call_expression function: (identifier) @function.call)
(call_expression function: (field_expression field: (identifier) @function.method.call))

(type_declaration name: (identifier) @type)
(named_type (identifier) @type)
(builtin_type) @type.builtin

(impl_declaration type: (named_type) @type)
(impl_declaration target: (named_type) @type)

; Members

(struct_field name: (identifier) @property)
(union_field name: (identifier) @property)
(tagged_union_member name: (identifier) @property)
(enum_variant name: (identifier) @constant)
(field_init name: (identifier) @property)
(field_expression field: (identifier) @property)

(dot_expression name: (identifier) @constant)
(variant_pattern name: (identifier) @constant)
(variant_constructor_expression variant: (identifier) @constant)

; Parameters and bindings

(parameter name: (identifier) @variable.parameter)
(method_parameter name: (identifier) @variable.parameter)
(extern_parameter name: (identifier) @variable.parameter)
(trait_parameter name: (identifier) @variable.parameter)
(macro_parameter name: (identifier) @variable.parameter)
(function_type_parameter name: (identifier) @variable.parameter)

(variable_declaration name: (identifier) @variable)
(variable_declaration_statement name: (identifier) @variable)
(for_binding name: (identifier) @variable)
(for_binding value_name: (identifier) @variable)
(variant_pattern binding: (identifier) @variable)

"self" @variable.builtin
(wildcard_pattern) @variable.builtin

; Keywords

[
  "fn"
  "ext"
  "type"
  "impl"
  "struct"
  "enum"
  "union"
  "trait"
  "macro"
] @keyword

[
  "if"
  "else"
  "while"
  "for"
  "switch"
  "match"
  "in"
  "defer"
  "return"
  "return_err"
  "return_ok"
  "try"
] @keyword.control

(break_statement) @keyword.control
(continue_statement) @keyword.control

["const" "mut" "pub" "packed"] @keyword.modifier

["size_of" "align_of" "type_of" "type_info_of" "len" "cast" "stackalloc" "import" "import_bin"] @function.builtin

; Operators and punctuation

[
  "="
  "+=" "-=" "*=" "/=" "&=" "|=" "^=" "<<=" ">>="
  "=="  "!=" "<" ">" "<=" ">="
  "&&" "||"
  "+" "-" "*" "/" "%"
  "&" "|" "^" "~" "!"
  "<<" ">>"
  "++" "--"
  "?"
  "->"
] @operator

["." ".." "..."] @punctuation.delimiter

["," ";" ":"] @punctuation.delimiter

["(" ")" "[" "]" "{" "}"] @punctuation.bracket

; Fallback - keep last so more specific patterns above win.
(identifier) @variable

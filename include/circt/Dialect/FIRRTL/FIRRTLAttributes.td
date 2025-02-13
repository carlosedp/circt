//===- FIRRTLAttributes.td - FIRRTL Types Definition -------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Definitions of FIRRTL attributes.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_FIRRTL_FIRRTLATTRIBUTES_TD
#define CIRCT_DIALECT_FIRRTL_FIRRTLATTRIBUTES_TD

include "mlir/IR/EnumAttr.td"
include "mlir/IR/BuiltinAttributeInterfaces.td"

//===----------------------------------------------------------------------===//
// FIRRTL Annotations Definition
//===----------------------------------------------------------------------===//

def AnnotationArrayAttr
  : TypedArrayAttrBase<DictionaryAttr, "Annotation array attribute">;

def PortAnnotationsAttr : ArrayAttrBase<
    And<[
      // Guarantee this is an ArrayAttr first
      CPred<"$_self.isa<::mlir::ArrayAttr>()">,
      // Guarantee all elements are an array or a dictionary.
      CPred<"::llvm::all_of($_self.cast<::mlir::ArrayAttr>(), "
            "[&](::mlir::Attribute attr) { return attr.isa<"
            "::mlir::ArrayAttr,"
            "::mlir::DictionaryAttr>();})">]>,
    "Port annotations attribute"> {
  let constBuilderCall = "$_builder.getArrayAttr($0)";
}

def InvalidValueAttr : AttrDef<FIRRTLDialect, "InvalidValue",
                               [TypedAttrInterface]> {
  let summary = "A constant value of firrtl.invalid type";
  let description = [{
    Represents an firrtl.invalidvalue value, whose type is specified by the
    type of the attribute.
  }];
  let mnemonic = "invalidvalue";
  let parameters = (ins  AttributeSelfTypeParameter<"">:$type);

  let assemblyFormat = "";

  let builders = [
    AttrBuilderWithInferredContext<(ins "Type":$type),
                                   "return $_get(type.getContext(), type);">
  ];
}

//===----------------------------------------------------------------------===//
// Grand Central-related Annotations
//===----------------------------------------------------------------------===//

class AugmentedType<string name> : AttrDef<FIRRTLDialect, name> {
  let description = "Used in the GrandCentralPass.";
  let parameters = (
    ins "DictionaryAttr":$underlying
  );

  code defaultClassDeclaration = [{
    StringAttr getClass() { return getUnderlying().getAs<StringAttr>("class"); }
  }];

  code hasName = [{
    StringAttr getName() { return getUnderlying().getAs<StringAttr>("name"); }
  }];

  code hasDefName = [{
    StringAttr getDefName() { return getUnderlying().getAs<StringAttr>("defName"); }
  }];

  code hasID = [{
    IntegerAttr getID() { return getUnderlying().getAs<IntegerAttr>("id"); }
  }];

  code hasElements = [{
    ArrayAttr getElements() { return getUnderlying().getAs<ArrayAttr>("elements"); }
  }];

  code hasPrefix = [{
    StringAttr getPrefix() { return getUnderlying().getAs<StringAttr>("prefix"); }
  }];

}

def AugmentedBundleType : AugmentedType<"AugmentedBundleType"> {
  let summary = "GrandCentral AugmentedBundleType";
  let extraClassDeclaration =
    defaultClassDeclaration #
    hasID #
    hasElements #
    hasDefName #
    hasPrefix # [{
      bool isRoot() { return getID() != nullptr; }
    }];
}

def AugmentedVectorType : AugmentedType<"AugmentedVectorType"> {
  let summary = "GrandCentral AugmentedVectorType";
  let extraClassDeclaration = defaultClassDeclaration # hasElements;
}

def AugmentedGroundType : AugmentedType<"AugmentedGroundType"> {
  let summary = "GrandCentral AugmentedGroundType";
  let extraClassDeclaration = hasID # hasName;
}

def AugmentedStringType : AugmentedType<"AugmentedStringType"> {
  let summary = "GrandCentral AugmentedStringType";
  let extraClassDeclaration = hasName;
}
def AugmentedBooleanType : AugmentedType<"AugmentedBooleanType"> {
  let summary = "GrandCentral AugmentedBooleanType";
  let extraClassDeclaration = hasName;
}
def AugmentedIntegerType : AugmentedType<"AugmentedIntegerType"> {
  let summary = "GrandCentral AugmentedIntegerType";
  let extraClassDeclaration = hasName;
}
def AugmentedDoubleType : AugmentedType<"AugmentedDoubleType"> {
  let summary = "GrandCentral AugmentedDoubleType";
  let extraClassDeclaration = hasName;
}
def AugmentedLiteralType : AugmentedType<"AugmentedLiteralType"> {
  let summary = "GrandCentral AugmentedLiteralType";
  let extraClassDeclaration = hasName;
}
def AugmentedDeletedType : AugmentedType<"AugmentedDeletedType"> {
  let summary = "GrandCentral AugmentedDeletedType";
  let extraClassDeclaration = hasName;
}


def ParamDeclAttr : AttrDef<FIRRTLDialect, "ParamDecl", [TypedAttrInterface]> {
  let summary = "Module or instance parameter definition";
  let description = [{
    An attribute describing a module parameter, or instance parameter
    specification.
  }];

  /// The value of the attribute - in a module, this is the default
  /// value (and may be missing).  In an instance, this is a required field that
  /// specifies the value being passed.  The verilog emitter omits printing the
  /// parameter for an instance when the applied value and the default value are
  /// the same.
  let parameters = (ins "::mlir::StringAttr":$name,
                        AttributeSelfTypeParameter<"">:$type,
                        "::mlir::Attribute":$value);
  let mnemonic = "param.decl";

  let hasCustomAssemblyFormat = 1;

  let builders = [
    AttrBuilderWithInferredContext<(ins "::mlir::StringAttr":$name,
                                        "::mlir::Type":$type),
      "auto *context = type.getContext();\n"
      "return $_get(context, name, type, Attribute());">,
    AttrBuilderWithInferredContext<(ins "::mlir::StringRef":$name,
                                        "::mlir::Type":$type),
      "return get(StringAttr::get(type.getContext(), name), type);">,

    AttrBuilderWithInferredContext<(ins "::mlir::StringAttr":$name,
                                        "::mlir::TypedAttr":$value),
      "auto *context = value.getContext();\n"
      "return $_get(context, name, value.getType(), value);">,
    AttrBuilderWithInferredContext<(ins "::mlir::StringRef":$name,
                                        "::mlir::TypedAttr":$value),
      "return get(StringAttr::get(value.getContext(), name), value);">
  ];

  let extraClassDeclaration = [{
    static ParamDeclAttr getWithName(ParamDeclAttr param,
                                     ::mlir::StringAttr name) {
      return get(param.getContext(), name, param.getType(), param.getValue());
    }
  }];
}

/// An array of ParamDeclAttr's that may or may not have a 'value' specified,
/// to be used on hw.module or hw.instance.  The hw.instance verifier further
/// ensures that all the values are specified.
def ParamDeclArrayAttr
  : TypedArrayAttrBase<ParamDeclAttr, "parameter array attribute">;

/// An attribute describing a kind of a name which defines name preservation
/// semantics. We preserve names tagged with `InterestingName`.
def DroppableName: I32EnumAttrCase<"DroppableName", 0, "droppable_name">;
def InterestingName: I32EnumAttrCase<"InterestingName", 1, "interesting_name">;

def NameKindEnumImpl: I32EnumAttr<"NameKindEnum", "name kind",
              [DroppableName, InterestingName]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::circt::firrtl";
}

def NameKindAttr: EnumAttr<FIRRTLDialect, NameKindEnumImpl, "name_kind">;

def InnerSymProperties : AttrDef<FIRRTLDialect, "InnerSymProperties"> {
  let mnemonic = "innerSymProps";
  let parameters = (ins
         "StringAttr":$name,
         DefaultValuedParameter<"int64_t", "0">:$fieldID,
         DefaultValuedParameter<"::mlir::StringAttr", "public">:$sym_visibility
                     );
  let builders = [
    AttrBuilderWithInferredContext<(ins "::mlir::StringAttr":$sym),[{
      return get(sym.getContext(), sym, 0,
                        StringAttr::get(sym.getContext(), "public") );
    }]>
  ];
  let hasCustomAssemblyFormat = 1;
  // The assembly format is as follows:
  // "`<` `@` $name `,` $fieldID `,` $sym_visibility `>`";
}

def InnerSymAttr : AttrDef<FIRRTLDialect, "InnerSym"> {
  let summary = "Inner symbol definition";
  let description = [{
    Defines the properties of an inner_sym attribute. It specifies the symbol
    name and symbol visibility for each field ID. For any ground types,
    there are no subfields and the field ID is 0. For aggregate types, a
    unique field ID is assigned to each field by visiting them in a 
    depth-first pre-order. The custom assembly format ensures that for ground
    types, only `@<sym_name>` is printed.
  }];
  let mnemonic = "innerSym";
  let parameters = (ins ArrayRefParameter<"InnerSymPropertiesAttr">:$props);
  let builders = [
    AttrBuilderWithInferredContext<(ins "::mlir::StringAttr":$sym),[{
      return get(sym.getContext(),
      {InnerSymPropertiesAttr::get(sym.getContext(), sym, 0,
                        StringAttr::get(sym.getContext(), "public"))});
    }]>,
    // Create an empty array, represents an invalid InnerSym.
    AttrBuilder<(ins),[{
      return get($_ctxt, {});
    }]>
  ];
  let extraClassDeclaration = [{
    /// Get the inner sym name for fieldID, if it exists.
    StringAttr getSymIfExists(unsigned fieldID) const;
    /// Get the inner sym name for fieldID=0, if it exists.
    StringAttr getSymName() const { return getSymIfExists(0); }
    /// Get the number of inner symbols defined.
    size_t size() const { return getProps().size(); }
    /// Check if this is an empty array, no sym names stored.
    bool empty() const { return getProps().empty(); }
    using iterator = ArrayRef<InnerSymPropertiesAttr>::iterator;
    /// Iterator begin for all the InnerSymProperties.
    iterator begin() const { return getProps().begin(); }
    /// Iterator end for all the InnerSymProperties.
    iterator end() const { return getProps().end(); }
    /// Invoke the func, for all sym names. Return success(),
    /// if the callback function never returns failure().
    LogicalResult walkSymbols(llvm::function_ref<
                              LogicalResult (StringAttr)>) const;
  }];

  let hasCustomAssemblyFormat = 1;
  // Example format:
  // firrtl.wire sym [<@x,1,private>, <@w,2,public>, <@syh,4,public>]
}

#endif // CIRCT_DIALECT_FIRRTL_FIRRTLATTRIBUTES_TD

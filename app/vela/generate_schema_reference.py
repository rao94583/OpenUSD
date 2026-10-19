"""Writes the usd-python skill's schema reference from this build's schema
registry: an index, and one Markdown file per schema library.

    generate_schema_reference.py OUTPUT_FOLDER
"""

import importlib
import os
import re
import sys

import pxr
from pxr import Plug, Sdf, Tf, Usd

# Python modules register their schema classes' Python types.
for module in pxr.__all__:
    try:
        importlib.import_module("pxr." + module)
    except ImportError:
        pass  # Usdviewq needs Qt

REGISTRY = Usd.SchemaRegistry()
KINDS = {
    Usd.SchemaKind.ConcreteTyped: "concrete",
    Usd.SchemaKind.AbstractTyped: "abstract",
    Usd.SchemaKind.SingleApplyAPI: "single-apply API",
    Usd.SchemaKind.MultipleApplyAPI: "multiple-apply API",
    Usd.SchemaKind.NonAppliedAPI: "non-applied API",
}
INSTANCE = "__INSTANCE_NAME__"


def summary(text, limit=220):
    """The first sentence of a schema's Doxygen documentation, as plain text."""
    text = re.sub(r"\\(em|b|c|p|a)\s+", "", text or "")
    text = re.sub(r"\\ref\s+(\S+)(\s+\"([^\"]*)\")?", lambda m: m.group(3) or m.group(1), text)
    text = re.sub(r"\\[a-z]+\b", "", text)
    text = re.sub(r"\s+", " ", text).strip()
    match = re.match(r"(.+?(?<!e\.g)(?<!i\.e)(?<!etc)[.!?])(\s|$)", text)
    sentence = match.group(1) if match else text
    if len(sentence) > limit:
        sentence = sentence[: limit - 1].rsplit(" ", 1)[0] + "…"
    return sentence.replace("|", "\\|")


def fallback(attribute):
    value = attribute.GetFallbackValue()
    if value is None:
        return ""
    text = str(value)
    if isinstance(value, str) and attribute.GetTypeName() in (Sdf.ValueTypeNames.Token, Sdf.ValueTypeNames.String):
        text = '"%s"' % value if value else '""'
    if len(text) > 40:
        text = text[:39] + "…"
    return "`%s`" % text.replace("|", "\\|")


def accessors(schema, attribute_names):
    """Maps attribute names to their Get...Attr methods, found by calling every
    getter on a prim that has the schema."""
    cls = schema["class"]
    if cls is None:
        return {}
    stage = Usd.Stage.CreateInMemory()
    kind = schema["kind"]
    try:
        if kind in ("concrete", "abstract"):
            concrete = schema["concrete"]
            if not concrete:
                return {}
            obj = cls(stage.DefinePrim("/P", concrete))
        elif kind == "single-apply API":
            obj = cls.Apply(stage.DefinePrim("/P"))
        elif kind == "multiple-apply API":
            obj = cls.Apply(stage.DefinePrim("/P"), "name")
        else:
            return {}
    except Exception:
        return {}
    found = {}
    for method in dir(cls):
        if not (method.startswith("Get") and method.endswith("Attr")):
            continue
        try:
            attribute = getattr(obj, method)()
        except Exception:
            continue
        if isinstance(attribute, Usd.Attribute) and attribute:
            name = attribute.GetName().replace(":name:", ":" + INSTANCE + ":")
            if name.startswith("name:"):
                name = INSTANCE + name[4:]
            found[name] = method
    return {name: found[name] for name in attribute_names if name in found}


def collect():
    base = Tf.Type.FindByName("UsdSchemaBase")
    schemas = {}
    for tf_type in base.GetAllDerivedTypes():
        name = Usd.SchemaRegistry.GetSchemaTypeName(tf_type)
        kind = KINDS.get(Usd.SchemaRegistry.GetSchemaKind(tf_type))
        if not name or not kind:
            continue
        plugin = Plug.Registry().GetPluginForType(tf_type)
        cls = tf_type.pythonClass
        module = cls.__module__.rsplit(".", 1)[-1] if cls else None
        library = module or (plugin.name if plugin else "Other")
        schemas[tf_type.typeName] = {
            "type": tf_type,
            "name": name,
            "kind": kind,
            "library": library,
            "class": cls,
            "python": "%s.%s" % (module, cls.__name__) if cls else None,
            "bases": [b.typeName for b in tf_type.baseTypes],
        }
    # Concrete descendants stand in for abstract schemas, which have no
    # prim definition.
    for schema in schemas.values():
        if schema["kind"] == "concrete":
            schema["concrete"] = schema["name"]
        elif schema["kind"] == "abstract":
            derived = [schemas[t.typeName]["name"] for t in schema["type"].GetAllDerivedTypes()
                       if t.typeName in schemas and schemas[t.typeName]["kind"] == "concrete"]
            schema["concrete"] = sorted(derived)[0] if derived else None
            schema["descendants"] = derived
    return schemas


def definition(schema):
    kind = schema["kind"]
    if kind in ("concrete", "abstract"):
        return REGISTRY.FindConcretePrimDefinition(schema["concrete"]) if schema["concrete"] else None
    if kind in ("single-apply API", "multiple-apply API"):
        return REGISTRY.FindAppliedAPIPrimDefinition(schema["name"])
    return None


def own_attributes(schema):
    cls = schema["class"]
    if cls is None or not hasattr(cls, "GetSchemaAttributeNames"):
        return []
    try:
        return list(cls.GetSchemaAttributeNames(False))
    except Exception:
        return []


def relationship_names(prim_definition):
    if prim_definition is None:
        return set()
    return {name for name in prim_definition.GetPropertyNames()
            if prim_definition.GetRelationshipDefinition(name)}


_relationships = {}


def all_relationships(schema):
    """Every relationship a schema's prims have; for an abstract schema, those
    all its concrete descendants share."""
    key = schema["type"].typeName
    if key not in _relationships:
        names = relationship_names(definition(schema))
        for other in schema.get("descendants", []):
            names &= relationship_names(REGISTRY.FindConcretePrimDefinition(other))
        _relationships[key] = names
    return _relationships[key]


def own_relationships(schema, schemas):
    """The relationships a schema adds: less its base's and its built-in API
    schemas'."""
    names = set(all_relationships(schema))
    for base in schema["bases"]:
        if base in schemas:
            names -= all_relationships(schemas[base])
    if schema["kind"] in ("concrete", "abstract") and schema["concrete"]:
        stand_in = REGISTRY.FindConcretePrimDefinition(schema["concrete"])
        for api in stand_in.GetAppliedAPISchemas():
            api_name, _, instance = api.partition(":")
            for name in relationship_names(REGISTRY.FindAppliedAPIPrimDefinition(api_name)):
                names.discard(name.replace(INSTANCE, instance))
    return names


def ancestors(schema, schemas):
    chain = []
    bases = schema["bases"]
    while bases and bases[0] in schemas:
        base = schemas[bases[0]]
        chain.append(base["name"])
        bases = base["bases"]
    return chain


_schema_layers = {}


def schema_doc(schema):
    """The class's documentation from its library's schema.usda, which the
    install tree keeps next to generatedSchema.usda (whose class docs are
    stripped)."""
    plugin = Plug.Registry().GetPluginForType(schema["type"])
    if plugin is None:
        return ""
    if plugin.name not in _schema_layers:
        path = os.path.join(plugin.resourcePath, plugin.name, "schema.usda")
        _schema_layers[plugin.name] = Sdf.Layer.FindOrOpen(path) if os.path.exists(path) else None
    layer = _schema_layers[plugin.name]
    spec = layer.GetPrimAtPath("/" + schema["name"]) if layer else None
    return summary(spec.documentation) if spec else ""


def write_schema(schema, schemas, out):
    prim_definition = definition(schema)
    doc = schema_doc(schema)
    out.append("## %s\n" % schema["name"])
    facts = []
    if schema["python"]:
        facts.append("`%s`" % schema["python"])
    facts.append(schema["kind"])
    chain = ancestors(schema, schemas)
    if chain and schema["kind"] in ("concrete", "abstract"):
        facts.append("base " + " → ".join(chain))
    out.append(" · ".join(facts) + "\n")
    if doc:
        out.append("\n" + doc + "\n")
    if prim_definition is not None and schema["kind"] == "concrete":
        apis = list(prim_definition.GetAppliedAPISchemas())
        if apis:
            out.append("\nBuilt-in API schemas: %s\n" % ", ".join("`%s`" % a for a in apis))

    attributes = own_attributes(schema)
    methods = accessors(schema, attributes)
    rows = []
    for name in attributes:
        attribute = prim_definition.GetAttributeDefinition(name) if prim_definition else None
        if attribute is None:
            continue
        type_name = str(attribute.GetTypeName())
        value = fallback(attribute)
        tokens = attribute.GetMetadata("allowedTokens")
        if tokens:
            value += (" " if value else "") + "(%s)" % ", ".join(str(t) for t in tokens)
        variability = " uniform" if attribute.GetVariability() == Sdf.VariabilityUniform else ""
        method = methods.get(name, "")
        rows.append("| `%s` | %s%s | %s | %s | %s |" % (
            name.replace(INSTANCE, "<name>"), type_name, variability, value,
            "`%s`" % method if method else "", summary(attribute.GetDocumentation(), 160)))
    if rows:
        out.append("\n| Attribute | Type | Fallback | Getter | About |\n|---|---|---|---|---|\n")
        out.append("\n".join(rows) + "\n")
    relationships = sorted(own_relationships(schema, schemas)) if schema["kind"] != "non-applied API" else []
    if relationships:
        out.append("\nRelationships: %s\n" % ", ".join(
            "`%s`" % r.replace(INSTANCE, "<name>") for r in relationships))
    out.append("\n")


ORDER = ["concrete", "abstract", "single-apply API", "multiple-apply API", "non-applied API"]


def main():
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    schemas = collect()
    libraries = {}
    for schema in schemas.values():
        libraries.setdefault(schema["library"], []).append(schema)

    index = [
        "# USD schemas in this build\n",
        "Generated from the schema registry of the USD that Vela runs. Each "
        "library's file lists its schemas with the attributes each one adds "
        "(not inherited ones; follow the base chain for those), their types, "
        "fallbacks, allowed tokens and Python getters. `Create<Name>Attr()` "
        "matches each `Get<Name>Attr()`.\n",
        "\n| Schema | Kind | File | About |\n|---|---|---|---|\n",
    ]
    for library in sorted(libraries, key=str.lower):
        members = sorted(libraries[library], key=lambda s: (ORDER.index(s["kind"]), s["name"]))
        out = ["# %s schemas\n\n" % library]
        for schema in members:
            write_schema(schema, schemas, out)
            index.append("| %s | %s | %s.md | %s |\n" % (
                schema["name"], schema["kind"], library,
                schema_doc(schema)[:120]))
        with open(os.path.join(folder, library + ".md"), "w") as f:
            f.write("".join(out).rstrip() + "\n")
    with open(os.path.join(folder, "index.md"), "w") as f:
        f.write("".join(index))


if __name__ == "__main__":
    main()

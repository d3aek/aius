"""Small, dependency-free evaluator for the Aius committed JSON Schema subset."""

from __future__ import annotations

import math
import re
from datetime import datetime
from urllib.parse import urlsplit


DRAFT_2020_12 = "https://json-schema.org/draft/2020-12/schema"

KEYWORDS = frozenset(
    {
        "$schema",
        "$id",
        "$defs",
        "$ref",
        "type",
        "required",
        "properties",
        "additionalProperties",
        "enum",
        "const",
        "pattern",
        "format",
        "items",
        "minItems",
        "maxItems",
        "uniqueItems",
        "minimum",
        "anyOf",
        "allOf",
        "contains",
        "minContains",
        "maxContains",
        "if",
        "then",
        "else",
    }
)

_JSON_TYPES = frozenset(
    {"null", "boolean", "object", "array", "number", "integer", "string"}
)
_FORMATS = frozenset({"date-time", "uri"})


class SchemaError(ValueError):
    """The schema is outside the supported, well-formed subset."""


class ValidationError(ValueError):
    """An instance does not satisfy a schema."""


def check_schema(schema: object) -> None:
    """Reject malformed schemas and every keyword outside ``KEYWORDS``."""

    _check_schema_node(schema, schema, "$schema")


def validate(instance: object, schema: object, *, root_schema: object | None = None) -> None:
    """Validate ``instance`` or raise ``ValidationError``.

    ``root_schema`` lets callers validate a definition while retaining the document
    root for local ``$ref`` resolution.
    """

    root = schema if root_schema is None else root_schema
    check_schema(root)
    _validate(instance, schema, root, "$")


def _check_schema_node(schema: object, root: object, path: str) -> None:
    if isinstance(schema, bool):
        return
    if not isinstance(schema, dict):
        raise SchemaError(f"{path}: schema must be an object or Boolean")

    unknown = sorted(set(schema) - KEYWORDS)
    if unknown:
        raise SchemaError(f"{path}: unrecognized schema keyword {unknown[0]!r}")

    if "$schema" in schema and schema["$schema"] != DRAFT_2020_12:
        raise SchemaError(f"{path}.$schema: only Draft 2020-12 is supported")
    if "$id" in schema and not isinstance(schema["$id"], str):
        raise SchemaError(f"{path}.$id: must be a string")

    definitions = schema.get("$defs")
    if definitions is not None:
        if not isinstance(definitions, dict):
            raise SchemaError(f"{path}.$defs: must be an object")
        for name, definition in definitions.items():
            _check_schema_node(definition, root, f"{path}.$defs.{name}")

    reference = schema.get("$ref")
    if reference is not None:
        if not isinstance(reference, str):
            raise SchemaError(f"{path}.$ref: must be a string")
        _resolve_ref(reference, root, path)

    declared_type = schema.get("type")
    if declared_type is not None:
        types = declared_type if isinstance(declared_type, list) else [declared_type]
        if not types or any(not isinstance(item, str) or item not in _JSON_TYPES for item in types):
            raise SchemaError(f"{path}.type: contains an unsupported JSON type")
        if len(types) != len(set(types)):
            raise SchemaError(f"{path}.type: types must be unique")

    required = schema.get("required")
    if required is not None:
        if (
            not isinstance(required, list)
            or any(not isinstance(item, str) for item in required)
            or len(required) != len(set(required))
        ):
            raise SchemaError(f"{path}.required: must be an array of unique strings")

    properties = schema.get("properties")
    if properties is not None:
        if not isinstance(properties, dict):
            raise SchemaError(f"{path}.properties: must be an object")
        for name, definition in properties.items():
            _check_schema_node(definition, root, f"{path}.properties.{name}")

    additional = schema.get("additionalProperties")
    if additional is not None and not isinstance(additional, (bool, dict)):
        raise SchemaError(f"{path}.additionalProperties: must be a schema or Boolean")
    if isinstance(additional, dict):
        _check_schema_node(additional, root, f"{path}.additionalProperties")

    enum = schema.get("enum")
    if enum is not None and (not isinstance(enum, list) or not enum):
        raise SchemaError(f"{path}.enum: must be a non-empty array")

    pattern = schema.get("pattern")
    if pattern is not None:
        if not isinstance(pattern, str):
            raise SchemaError(f"{path}.pattern: must be a string")
        try:
            re.compile(pattern)
        except re.error as error:
            raise SchemaError(f"{path}.pattern: invalid regular expression: {error}") from error

    format_name = schema.get("format")
    if format_name is not None and format_name not in _FORMATS:
        raise SchemaError(f"{path}.format: unsupported format {format_name!r}")

    items = schema.get("items")
    if items is not None:
        _check_schema_node(items, root, f"{path}.items")

    for keyword in ("minItems", "maxItems", "minContains", "maxContains"):
        value = schema.get(keyword)
        if value is not None and (not isinstance(value, int) or isinstance(value, bool) or value < 0):
            raise SchemaError(f"{path}.{keyword}: must be a non-negative integer")
    unique = schema.get("uniqueItems")
    if unique is not None and not isinstance(unique, bool):
        raise SchemaError(f"{path}.uniqueItems: must be Boolean")

    minimum = schema.get("minimum")
    if minimum is not None and not _is_number(minimum):
        raise SchemaError(f"{path}.minimum: must be a finite number")

    for keyword in ("anyOf", "allOf"):
        alternatives = schema.get(keyword)
        if alternatives is not None:
            if not isinstance(alternatives, list) or not alternatives:
                raise SchemaError(f"{path}.{keyword}: must be a non-empty schema array")
            for index, alternative in enumerate(alternatives):
                _check_schema_node(alternative, root, f"{path}.{keyword}[{index}]")

    for keyword in ("contains", "if", "then", "else"):
        subschema = schema.get(keyword)
        if subschema is not None:
            _check_schema_node(subschema, root, f"{path}.{keyword}")


def _validate(instance: object, schema: object, root: object, path: str) -> None:
    if schema is True:
        return
    if schema is False:
        raise ValidationError(f"{path}: rejected by false schema")
    if not isinstance(schema, dict):
        raise SchemaError(f"{path}: schema must be an object or Boolean")

    if "$ref" in schema:
        target = _resolve_ref(schema["$ref"], root, path)
        _validate(instance, target, root, path)

    if "type" in schema:
        declared = schema["type"]
        types = declared if isinstance(declared, list) else [declared]
        if not any(_matches_type(instance, item) for item in types):
            raise ValidationError(f"{path}: expected type {' or '.join(types)}")

    if "enum" in schema and not any(_json_equal(instance, item) for item in schema["enum"]):
        raise ValidationError(f"{path}: value is not in enum")
    if "const" in schema and not _json_equal(instance, schema["const"]):
        raise ValidationError(f"{path}: value does not equal const")

    if isinstance(instance, str):
        if "pattern" in schema and re.search(schema["pattern"], instance) is None:
            raise ValidationError(f"{path}: string does not match pattern")
        if "format" in schema and not _matches_format(instance, schema["format"]):
            raise ValidationError(f"{path}: string does not match {schema['format']} format")

    if isinstance(instance, dict):
        required = schema.get("required", [])
        for name in required:
            if name not in instance:
                raise ValidationError(f"{path}: missing required property {name!r}")

        properties = schema.get("properties", {})
        for name, value in instance.items():
            child_path = f"{path}.{name}"
            if name in properties:
                _validate(value, properties[name], root, child_path)
            elif "additionalProperties" in schema:
                additional = schema["additionalProperties"]
                if additional is False:
                    raise ValidationError(f"{path}: additional property {name!r} is not allowed")
                if additional is not True:
                    _validate(value, additional, root, child_path)

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            raise ValidationError(f"{path}: has fewer than {schema['minItems']} items")
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            raise ValidationError(f"{path}: has more than {schema['maxItems']} items")
        if schema.get("uniqueItems"):
            for left in range(len(instance)):
                for right in range(left + 1, len(instance)):
                    if _json_equal(instance[left], instance[right]):
                        raise ValidationError(f"{path}: items {left} and {right} are not unique")
        if "items" in schema:
            for index, item in enumerate(instance):
                _validate(item, schema["items"], root, f"{path}[{index}]")
        if "contains" in schema:
            matches = 0
            for item in instance:
                try:
                    _validate(item, schema["contains"], root, path)
                except ValidationError:
                    continue
                matches += 1
            minimum = schema.get("minContains", 1)
            maximum = schema.get("maxContains")
            if matches < minimum:
                raise ValidationError(f"{path}: contains matched {matches}, fewer than {minimum}")
            if maximum is not None and matches > maximum:
                raise ValidationError(f"{path}: contains matched {matches}, more than {maximum}")

    if _is_number(instance) and "minimum" in schema and instance < schema["minimum"]:
        raise ValidationError(f"{path}: value is less than minimum")

    if "anyOf" in schema:
        if not any(_is_valid(instance, alternative, root, path) for alternative in schema["anyOf"]):
            raise ValidationError(f"{path}: value does not satisfy anyOf")
    if "allOf" in schema:
        for alternative in schema["allOf"]:
            _validate(instance, alternative, root, path)

    if "if" in schema:
        branch = "then" if _is_valid(instance, schema["if"], root, path) else "else"
        if branch in schema:
            _validate(instance, schema[branch], root, path)


def _is_valid(instance: object, schema: object, root: object, path: str) -> bool:
    try:
        _validate(instance, schema, root, path)
    except ValidationError:
        return False
    return True


def _resolve_ref(reference: str, root: object, path: str) -> object:
    fragment = reference
    if not fragment.startswith("#"):
        root_id = root.get("$id") if isinstance(root, dict) else None
        if isinstance(root_id, str) and fragment.startswith(f"{root_id}#"):
            fragment = fragment[len(root_id) :]
        else:
            raise SchemaError(f"{path}: only references within the root schema are supported")
    if fragment == "#":
        return root
    if not fragment.startswith("#/"):
        raise SchemaError(f"{path}: unsupported reference {reference!r}")

    current = root
    for raw_token in fragment[2:].split("/"):
        token = raw_token.replace("~1", "/").replace("~0", "~")
        if isinstance(current, dict) and token in current:
            current = current[token]
        elif isinstance(current, list) and token.isdigit() and int(token) < len(current):
            current = current[int(token)]
        else:
            raise SchemaError(f"{path}: reference does not resolve: {reference!r}")
    return current


def _matches_type(value: object, type_name: str) -> bool:
    if type_name == "null":
        return value is None
    if type_name == "boolean":
        return isinstance(value, bool)
    if type_name == "object":
        return isinstance(value, dict)
    if type_name == "array":
        return isinstance(value, list)
    if type_name == "number":
        return _is_number(value)
    if type_name == "integer":
        if isinstance(value, int) and not isinstance(value, bool):
            return True
        return isinstance(value, float) and math.isfinite(value) and value.is_integer()
    if type_name == "string":
        return isinstance(value, str)
    raise SchemaError(f"unsupported JSON type {type_name!r}")


def _is_number(value: object) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return True
    return isinstance(value, float) and math.isfinite(value)


def _json_equal(left: object, right: object) -> bool:
    if _is_number(left) and _is_number(right):
        return left == right
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(
            _json_equal(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _json_equal(left_item, right_item)
            for left_item, right_item in zip(left, right)
        )
    return left == right


def _matches_format(value: str, format_name: str) -> bool:
    if format_name == "date-time":
        if re.fullmatch(
            r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})",
            value,
        ) is None:
            return False
        try:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError:
            return False
        return parsed.tzinfo is not None
    if format_name == "uri":
        if re.search(r"\s|%(?![0-9A-Fa-f]{2})", value):
            return False
        try:
            parsed = urlsplit(value)
        except ValueError:
            return False
        if not parsed.scheme:
            return False
        return parsed.scheme not in {"http", "https"} or bool(parsed.netloc)
    raise SchemaError(f"unsupported format {format_name!r}")

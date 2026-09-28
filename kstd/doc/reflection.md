# Runtime reflection

`std.meta` declares compile-time handles (`Symbol`, `Type`, `Class`, `Field`,
`Function`, and `Parameter`). `meta(...)` is evaluated by the compiler;
these handles are not runtime descriptors. The five specific handles inherit
from `Symbol`, which owns their shared `id` and `name: std.util.string.StringSlice`.
`std.reflect` owns the runtime
`Type`, `Field`, and `Value` APIs. Its `Class`, `Function`, and `Parameter`
types reserve names for later descriptor versions; KLRF v1 exports only
selected fields. Field, function return, and parameter types are represented
by `Type` values in both modules; raw descriptor IDs remain an ABI detail.

`@reflect` selects public instance fields of a class and fields individually
marked `@reflect`, including private fields. Other fields are absent from the
runtime table.

```kelyra
import std.reflect;

@reflect
class Player {
  pub health: i32;
  @reflect private_id: u64;
}

let player = Player();
let info = std.reflect.type_of<Player>();
if info.has_field("health") {
  let found = info.field("health"); // Result<std.reflect.Field, i32>
  if found.valid && found.ok {
    let field = found.value();
    let name: []const u8 = field.name();
    let object = std.reflect.value<Player>(&player);
    let previous = std.reflect.read<i32>(field, object);
    let written = std.reflect.write<i32>(field, object, 80);
  }
}
```

`Type.name()` and `Field.name()` return borrowed `std.util.string.StringSlice`
values (`[]const u8`). `std.meta.Id` and `std.reflect.Id` are separate aliases
of `usize`; their values belong to different compilation phases.
`Type.has_field_bytes()` and `field_bytes()` accept a `[]const u8` name;
`has_field()` and `field()` accept a NUL-terminated `*c.char` for string
literals. `std.util.string.from_cstr()` converts a C string to a borrowed slice,
and `std.util.string.to_owned()` copies a slice into an owning `String`.

`Result<T, i32>.error()` is `0` for an absent field and nonzero for an error.
Runtime read and write use error codes `1` (wrong owner type), `2` (wrong field
type), and `3` (null object pointer). Check `valid` before `ok`, then call
`value()` for success or `error()` for failure. `valid == false` indicates
allocation failure. `write` uses normal class assignment, including copy/move
and destruction; `read` returns a copy of the field.

`Type.field_count()` counts selected fields. `Field.owner` is a `Class` whose
`type` identifies the declaring class. `Class.name()` and `same_class()`
provide direct queries; `Field.field_type` is a `Type`.
`Field.type()` returns the latter. `Type.same_type()`
compares identity and name. A field type has no recursive field table. `type_of<T>()`
provides a field table only when T is annotated. KLRF v1 cannot distinguish a
type without exported fields from an exported type with an empty table.

The compiler emits one read-only descriptor in the object file that defines
each reflected class. Consumers can use `type_of<T>()` while linking a
precompiled library; they still need the class declaration source for static
type checking. No C implementation or host CRT is required.

Descriptor ABI v1 uses an exported `__kelyra_reflect_v1_...` symbol per type.
Its byte header contains `KLRF`, version `1`, a 64-bit type ID, the type-name
length, and the selected-field count. Each field stores its type ID, byte
offset, name length, type-name length, and null-terminated names. The standard
library checks the magic and version before reading a descriptor. Rebuild a
library after an incompatible compiler ABI change.

The separation between compile-time metadata and runtime reflection is
recorded in [the reflection architecture](../../compiler/doc/reflection-architecture.md).

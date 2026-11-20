# SQL/PGQ grammar extension

DuckPGQ adds its SQL/PGQ syntax through the `GrammarExtension` API of DuckDB v2.0-cyanoptera: `CREATE`, `ALTER`, `DROP`,
`DESCRIBE`/`SHOW`, and `SUMMARIZE PROPERTY GRAPH`, and `GRAPH_TABLE`. DuckDB itself is not patched, no parser code is
vendored, and the submodule points at upstream `duckdb/duckdb`.

## Activation

Loading DuckPGQ registers the `duckpgq` grammar extension. Activation is explicit and connection-local:

```sql
LOAD duckpgq;
SET active_grammar_extensions = ['duckpgq'];

CREATE TABLE vertices(id BIGINT, name VARCHAR);
CREATE PROPERTY GRAPH example VERTEX TABLES (vertices);
```

If other grammar extensions are in use, include their names in the same list. `RESET active_grammar_extensions`
restores the base grammar. Loading DuckPGQ no longer changes `allow_parser_override_extension` or intercepts every
SQL query. A new connection must activate the grammar independently.

Execute activation before sending queries containing PGQ syntax: DuckDB parses a query batch before executing
its `SET` statement. SQL `PREPARE` is currently unavailable for extension statements.

## Supported syntax

### CREATE PROPERTY GRAPH

- `CREATE PROPERTY GRAPH`, `CREATE OR REPLACE PROPERTY GRAPH`, and `CREATE PROPERTY GRAPH IF NOT EXISTS`.
  `OR REPLACE` and `IF NOT EXISTS` cannot be combined.
- A required non-empty `VERTEX TABLES (...)` clause and an optional non-empty `EDGE TABLES (...)` clause.
- Quoted and qualified table names; `AS` table aliases; explicit labels and discriminator sublabels.
- Omitted properties, `ALL PROPERTIES`, `NO PROPERTIES`, `PROPERTIES [ARE] ALL COLUMNS`,
  `PROPERTIES [ARE] ALL COLUMNS EXCEPT (...)`, and explicit property lists. An `AS` alias on a property is parsed
  and stored, but the match rewrite does not yet resolve it: patterns must use the column name.
- Explicit source/destination key references, including composite keys, or short references inferred from table
  primary/foreign-key constraints.
- Existing create-graph binding, validation, metadata storage, and conflict policies.

Graph names are unique per database, and every graph records the catalog and schema it was created in (the
current catalog and `main` when unqualified; the schema must exist). A qualified reference anywhere, in
`GRAPH_TABLE`, `DROP`, `DESCRIBE`, or `SUMMARIZE`, must match that catalog and schema, so `DROP PROPERTY GRAPH
other.g` can never remove `main.g`. Table/reference qualification is preserved. Temporary property graphs,
vertex-key declarations, and arbitrary property expressions are not supported.

Edge metadata stores resolved physical vertex names and qualification rather than statement-local aliases, so
new connections can reconstruct graphs created with aliased references.

### ALTER PROPERTY GRAPH

The actions of SQL/PGQ 11.20 that fit DuckPGQ's model of one main label (plus discriminator sublabels) per element
table:

```sql
ALTER PROPERTY GRAPH g ADD VERTEX TABLES (...) [ADD EDGE TABLES (...)];
ALTER PROPERTY GRAPH g ADD EDGE TABLES (...);
ALTER PROPERTY GRAPH g DROP {VERTEX | EDGE} TABLES (t, ...) [CASCADE | RESTRICT];
ALTER PROPERTY GRAPH g ALTER {VERTEX | EDGE} TABLE t ALTER LABEL l ADD PROPERTIES (column [AS name], ...);
ALTER PROPERTY GRAPH g ALTER {VERTEX | EDGE} TABLE t ALTER LABEL l DROP PROPERTIES (name, ...) [CASCADE | RESTRICT];
```

- Added element tables take the same definitions as in CREATE and are validated the same way. Edge tables may
  reference vertex tables already in the graph. Labels stay unique.
- Element tables are named by their table name `[[catalog.]schema.]t`, in `DROP ... TABLES`, `ALTER ... TABLE` and in
  edge references to vertex tables already in the graph. An unqualified name matches the element tables with that
  table name (11.21 SR 4b), whatever the catalog would resolve it to; when tables of several schemas or catalogs share
  the name, it is ambiguous and must be qualified. A qualified name matches the element table over the table it resolves to, so
  `main.t` and `memory.main.t` name the same one. Aliases are not persisted, so they do not name element tables (an
  edge added in the same statement may still reference a vertex table added there by its alias). A table can be a
  vertex (or edge) table of a graph only once; a graph created with the same table twice can be extended, but the
  repeated table cannot be named, so it cannot be dropped or altered.
- `DROP VERTEX TABLES` is `RESTRICT` by default: it fails while an edge table references the vertex table. `CASCADE`
  also drops those edge tables. The last vertex table cannot be dropped; use `DROP PROPERTY GRAPH`.
- Properties belong to the element table, so `l` may be the table's main label or one of its sublabels and the change
  applies to all of them. Property names must be unique per element table; one column may be exposed under several
  names. MATCH, however, still resolves properties by column name, as for CREATE (cwida/duckpgq-extension#364).
- `CASCADE` and `RESTRICT` on `DROP PROPERTIES` behave the same: dependencies of views on a property (or on an element
  table) are not tracked, so `RESTRICT` does not fail for a view that uses it, and the view fails when it is next
  queried. `DROP PROPERTY GRAPH` does not track views either.
- Edge tables added by ALTER are checked for incomplete edges like in CREATE. A graph that is not verified (an edge
  table could not be checked) has all its edge tables checked again on every ALTER, so it becomes verified once those
  tables are readable or dropped; an incomplete edge in an existing edge table then keeps it unverified rather than
  failing the ALTER.
- ALTER is not transactional, like CREATE and DROP PROPERTY GRAPH: the new definition is registered in every
  connection and written to `__duckpgq_internal` on a separate connection, so a `ROLLBACK` does not undo it, and an
  element table over a table created in the rolled-back transaction is left dangling. Concurrent ALTERs of one graph
  are not serialised; the last one to finish wins.
- `ADD LABEL` and `DROP LABEL` are not supported. The statement is transformed into an `AlterPropertyGraphInfo`
  (derived from `CreatePropertyGraphInfo`) and planned onto `AlterPropertyGraphFunction`, which builds the altered
  definition from the registered graph and stores it the way `CREATE OR REPLACE` does.

### GRAPH_TABLE

- `GRAPH_TABLE (...)` and `GRAPH TABLE (...)` as a table reference anywhere DuckDB accepts one: FROM lists, joins,
  CTEs, subqueries, `CREATE TABLE AS`, `INSERT ... SELECT`, `COPY`, `EXPLAIN`, and prepared statements. Views over
  `GRAPH_TABLE` work in in-memory databases only: a file-backed database cannot serialize the pattern carried in
  `bind_info` and rejects the `CREATE VIEW` at commit. The view's reader also needs the grammar active.
- Path patterns with vertex and edge elements, `:label` or `IS label`, element-local and pattern-level `WHERE`, and
  an optional `COLUMNS (...)`. Without `COLUMNS`, every vertex binding's columns are exposed.
- Edge directions `-[e]->`, `<-[e]-`, `-[e]-`, and the spaced spellings the old parser accepted.
- Quantifiers `*`, `+`, `?`, `{n}`, `{m,n}`, `{m,}`, `{,n}`, including the forms the tokenizer fuses with the arrow
  (`->*`, `->?`, `-*`, `-?`).
- `?` is an optional match, not the quantifier `{0,1}`: the edge and the vertex after it are `LEFT JOIN`ed onto the
  rest of the match, so rows without such an edge are kept with NULL for both variables. Element-local `WHERE`s in
  the optional part go into the join condition; the pattern-level `WHERE` filters the result. The optional edge must
  be the last edge of its path pattern, its edge and vertex variables cannot occur elsewhere in the MATCH, and it
  cannot be under a path variable. Several optional edges from one vertex are written as separate path patterns:
  `(a:Student)-[s:studyAt]->?(u:University), (a:Student)-[w:workAt]->?(c:Company)`.
- Path variables, `ANY SHORTEST`, `ALL SHORTEST`, `SHORTEST k`, and the `WALK`/`TRAIL`/`SIMPLE`/`ACYCLIC` modes.
  Which of these the match rewrite can execute is decided in the rewrite, not the grammar.

### Graph-management statements

- `DROP PROPERTY GRAPH [IF EXISTS] name` transforms to the existing `DropPropertyGraphInfo` inside an
  `ExtensionStatement`, planned onto the `drop_property_graph` table function like CREATE.
- `DESCRIBE`, `SHOW`, and `SUMMARIZE PROPERTY GRAPH name` join DuckDB's native `DescribeStatement` family and rewrite
  to `SELECT * FROM describe_property_graph('name')` or `summarize_property_graph('name')`. They therefore also work
  as subqueries, e.g. `FROM (DESCRIBE PROPERTY GRAPH g)`. `describe_property_graph` now takes the graph name as its
  argument instead of reading parser state.

The GRAPH_TABLE transform builds the existing `MatchExpression` and returns a native `TableFunctionRef` to
`duckpgq_match`.
The pattern travels in `TableFunctionRef::bind_info`, DuckDB's process-local slot that survives statement copies,
so it is never bound as a scalar argument and prepared statements can be rebound. `duckpgq_match` takes no
arguments and its `bind_replace` reads the pattern from `bind_input.ref.bind_info`, rewriting a copy of it.
The old plan-time interception of SELECT statements and the operator-extension bind hook are removed.

## Integration

The `.gram` files under `src/core/parser/grammar/` define the namespaced `DuckPGQ*` rules, one complete rule per
non-comment line. CMake embeds each file in a generated header under the build directory (`duckpgq_embed_grammar`
in `src/core/parser/CMakeLists.txt`) and reconfigures when a `.gram` file changes. No generation script is required.

`create_property_graph_grammar.cpp`, `graph_table_grammar.cpp`, and `graph_management_grammar.cpp` register their
rules, attach handwritten transform callbacks with `SetTransformProcess`, and prepend their entry rules to the native
`Statement`, `InnerTableRef`, and `DescribeStatement` choices. `duckpgq_grammar_extension.cpp` combines them into the
single `duckpgq` grammar extension. Built-in
`QualifiedName`, `BaseTableName`, `TableAlias`, `ColId`, `Identifier`, `NumberLiteral`, `WhereClause`, `TargetList`,
`Parens`, and `List` rules/transforms are reused rather than copied. Keyword literals that are not DuckDB keywords
match by text, so no reserved words are added; the create-graph words are registered as unreserved keywords.

The rule transforms to the existing `CreatePropertyGraphInfo`, wrapped in a native `ExtensionStatement` carrying
`DuckPGQParseData`. The existing parser-extension plan callback invokes the create-graph table function. The parser
extension is retained for planning only: no parser override or parse fallback is registered. DuckDB handles
tokenization, statement boundaries, native SQL transforms, and syntax-error locations.

## Verification

Build static/loadable extensions, the shell, and the test runner, then run:

```sh
./build/release/test/unittest 'test/sql/grammar_extension/*'
```

The focused suite verifies activation/reset and connection isolation, ordinary SQL coexistence, mixed statements,
quoted/schema-qualified names, table/property aliases, property modes, labels, inferred/explicit references,
conflict policies, the SQL `PREPARE` limitation, GRAPH_TABLE in every table-reference position, the graph-management
statements, and syntax/semantic errors.

The full suite (`./build/release/test/unittest 'test/sql/*'`) passes. Every legacy test activates the grammar right
after its `require` lines, once per connection it uses, because activation is connection-local. Expected
parser-error messages in `parser_arrow_kleene.test` no longer pin the offending token, since DuckDB's tokenizer
decides token boundaries (for example `/->` or `<->` are single operator tokens).

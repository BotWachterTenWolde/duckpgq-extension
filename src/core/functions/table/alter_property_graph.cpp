#include "duckpgq/core/functions/table/alter_property_graph.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckpgq/core/functions/table/create_property_graph.hpp"
#include <duckpgq/core/parser/duckpgq_parser.hpp>
#include <duckpgq/core/utils/duckpgq_utils.hpp>

namespace duckdb {

namespace {

const char *ElementKind(bool vertex) {
	return vertex ? "vertex" : "edge";
}

vector<shared_ptr<PropertyGraphTable>> &ElementTables(CreatePropertyGraphInfo &graph, bool vertex) {
	return vertex ? graph.vertex_tables : graph.edge_tables;
}

//! The table a name resolves to, or nullptr when there is none: also when the name is a view or its catalog is gone.
optional_ptr<TableCatalogEntry> LookupTable(ClientContext &context, const QualifiedName &name) {
	optional_ptr<CatalogEntry> entry;
	try {
		entry =
		    Catalog::GetEntry(context, EntryLookupInfo(CatalogType::TABLE_ENTRY, name), OnEntryNotFound::RETURN_NULL);
	} catch (CatalogException &) {
		return nullptr;
	} catch (BinderException &) {
		return nullptr;
	}
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<TableCatalogEntry>();
}

optional_ptr<TableCatalogEntry> LookupTable(ClientContext &context, const PropertyGraphTable &table) {
	return LookupTable(context, QualifiedName(table.catalog_name, table.schema_name, table.table_name));
}

//! Binds written qualifiers the way DuckDB binds table names, so that db2.person and memory.main.person compare to the
//! bound names stored in the graph.
void BindQualifiers(ClientContext &context, Identifier &catalog, Identifier &schema) {
	try {
		Binder::BindSchemaOrCatalog(context, catalog, schema);
	} catch (CatalogException &) {
	} catch (BinderException &) {
	}
}

//! The element tables of [tables] that a name as written matches. An unqualified name matches by table name (SQL/PGQ
//! 11.21 SR 4b); a qualified one by the table it resolves to, or by its bound qualifiers when that table is gone.
vector<shared_ptr<PropertyGraphTable>> MatchElementTables(ClientContext &context,
                                                          const vector<shared_ptr<PropertyGraphTable>> &tables,
                                                          Identifier catalog, Identifier schema,
                                                          const Identifier &name) {
	vector<shared_ptr<PropertyGraphTable>> result;
	if (catalog.empty() && schema.empty()) {
		for (auto &table : tables) {
			if (table->table_name == name) {
				result.push_back(table);
			}
		}
		return result;
	}
	BindQualifiers(context, catalog, schema);
	auto entry = LookupTable(context, QualifiedName(catalog, schema, name));
	for (auto &table : tables) {
		if (entry ? LookupTable(context, *table).get() == entry.get()
		          : (table->catalog_name == catalog && table->schema_name == schema && table->table_name == name)) {
			result.push_back(table);
		}
	}
	return result;
}

//! Element tables are named by their table name, qualified where that is ambiguous. Aliases are not persisted, so they
//! cannot name an element table reliably: another connection's ALTER, or a reload, replaces the definition.
shared_ptr<PropertyGraphTable> FindElementTable(ClientContext &context, CreatePropertyGraphInfo &graph,
                                                const QualifiedName &name, bool vertex) {
	auto matches =
	    MatchElementTables(context, ElementTables(graph, vertex), name.Catalog(), name.Schema(), name.Name());
	if (matches.size() > 1) {
		throw BinderException(
		    "%s table name %s is ambiguous in property graph %s; qualify it with its schema or catalog",
		    ElementKind(vertex), name.Name(), graph.property_graph_name);
	}
	if (matches.empty()) {
		throw BinderException("%s table %s is not part of property graph %s", ElementKind(vertex), name.Name(),
		                      graph.property_graph_name);
	}
	return matches[0];
}

//! The element tables named by a DROP list, which must be distinct.
vector<shared_ptr<PropertyGraphTable>> FindElementTables(ClientContext &context, AlterPropertyGraphInfo &info,
                                                         CreatePropertyGraphInfo &graph, bool vertex) {
	vector<shared_ptr<PropertyGraphTable>> result;
	for (auto &name : info.element_tables) {
		auto table = FindElementTable(context, graph, name, vertex);
		if (std::find(result.begin(), result.end(), table) != result.end()) {
			throw BinderException("%s table %s is listed more than once", vertex ? "Vertex" : "Edge",
			                      table->table_name);
		}
		result.push_back(table);
	}
	return result;
}

//! Names listed in a DROP PROPERTIES list must be distinct.
void CheckDistinct(const vector<Identifier> &names, const char *what) {
	for (idx_t i = 0; i < names.size(); i++) {
		for (idx_t j = 0; j < i; j++) {
			if (names[i] == names[j]) {
				throw BinderException("%s %s is listed more than once", what, names[i]);
			}
		}
	}
}

//! Whether two element tables are over the same table, however their names were qualified.
bool SameTable(ClientContext &context, const PropertyGraphTable &left, const PropertyGraphTable &right) {
	auto left_entry = LookupTable(context, left);
	auto right_entry = LookupTable(context, right);
	if (left_entry && right_entry) {
		return left_entry.get() == right_entry.get();
	}
	return left.SameTableIdentity(right);
}

void RemoveTable(vector<shared_ptr<PropertyGraphTable>> &tables, const shared_ptr<PropertyGraphTable> &table) {
	tables.erase(std::remove(tables.begin(), tables.end(), table), tables.end());
}

//! Rebuilds the label map of the altered graph; labels must stay unique across all element tables.
void RebuildLabelMap(CreatePropertyGraphInfo &graph) {
	graph.label_map.clear();
	auto register_table = [&](const shared_ptr<PropertyGraphTable> &table) {
		auto register_label = [&](const Identifier &label) {
			auto name = label.GetIdentifierName();
			if (!graph.label_map.emplace(name, table).second) {
				throw ConstraintException("Label %s is not unique, make sure all labels are unique",
				                          StringUtil::Lower(name));
			}
		};
		register_label(table->main_label);
		for (auto &label : table->sub_labels) {
			register_label(label);
		}
	};
	for (auto &table : graph.vertex_tables) {
		register_table(table);
	}
	for (auto &table : graph.edge_tables) {
		register_table(table);
	}
}

//! Links an added edge table's reference to a vertex table of the altered graph. A reference to a vertex table added
//! by the same statement (also by its alias) was linked by the transform; any other reference is matched against the
//! vertex tables of the graph.
void ResolveEdgeReference(ClientContext &context, CreatePropertyGraphInfo &graph,
                          const unordered_map<PropertyGraphTable *, shared_ptr<PropertyGraphTable>> &added_vertices,
                          shared_ptr<PropertyGraphTable> &vertex_table, Identifier &catalog, Identifier &schema,
                          Identifier &reference) {
	if (vertex_table) {
		auto entry = added_vertices.find(vertex_table.get());
		vertex_table = entry == added_vertices.end() ? nullptr : entry->second;
	}
	if (!vertex_table) {
		auto matches = MatchElementTables(context, graph.vertex_tables, catalog, schema, reference);
		if (matches.size() > 1) {
			throw BinderException(
			    "Vertex table name %s is ambiguous in property graph %s; qualify it with its schema or catalog",
			    reference, graph.property_graph_name);
		}
		if (!matches.empty()) {
			vertex_table = matches[0];
		}
	}
	if (!vertex_table) {
		auto full_name = (catalog.empty() ? "" : catalog.GetIdentifierName() + ".") +
		                 (schema.empty() ? "" : schema.GetIdentifierName() + ".") + reference.GetIdentifierName();
		throw Exception(ExceptionType::INVALID,
		                "Table '" + full_name + "' not found in the property graph " + graph.property_graph_name + ".");
	}
	catalog = vertex_table->catalog_name;
	schema = vertex_table->schema_name;
	reference = vertex_table->table_name;
}

void AddTables(ClientContext &context, AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph,
               vector<shared_ptr<PropertyGraphTable>> &added_edges) {
	vector<shared_ptr<PropertyGraphTable>> added_vertices;
	unordered_map<PropertyGraphTable *, shared_ptr<PropertyGraphTable>> statement_vertices;
	for (auto &vertex_table : info.vertex_tables) {
		auto table = vertex_table->Copy();
		statement_vertices[vertex_table.get()] = table;
		added_vertices.push_back(table);
		graph.vertex_tables.push_back(table);
	}
	for (auto &edge_table : info.edge_tables) {
		auto table = edge_table->Copy();
		ResolveEdgeReference(context, graph, statement_vertices, table->source_pg_table, table->source_catalog,
		                     table->source_schema, table->source_reference);
		ResolveEdgeReference(context, graph, statement_vertices, table->destination_pg_table,
		                     table->destination_catalog, table->destination_schema, table->destination_reference);
		added_edges.push_back(table);
		graph.edge_tables.push_back(table);
	}
	RebuildLabelMap(graph);
	CreatePropertyGraphFunction::ValidateTables(context, graph, added_vertices, added_edges);
	// Element tables are named by table name (aliases are not persisted) and edges store the physical vertex name, so a
	// table can be an element of a kind only once; a second one could not be told apart later.
	auto check_new = [&](const vector<shared_ptr<PropertyGraphTable>> &added, bool vertex) {
		for (auto &table : added) {
			for (auto &other : ElementTables(graph, vertex)) {
				if (other != table && SameTable(context, *other, *table)) {
					throw BinderException("Table %s is already %s %s table of property graph %s", table->table_name,
					                      vertex ? "a" : "an", ElementKind(vertex), graph.property_graph_name);
				}
			}
		}
	};
	check_new(added_vertices, true);
	check_new(added_edges, false);
}

void DropVertexTables(ClientContext &context, AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph) {
	for (auto &vertex_table : FindElementTables(context, info, graph, true)) {
		vector<shared_ptr<PropertyGraphTable>> dependents;
		for (auto &edge_table : graph.edge_tables) {
			if (edge_table->source_pg_table == vertex_table || edge_table->destination_pg_table == vertex_table) {
				dependents.push_back(edge_table);
			}
		}
		if (!dependents.empty() && !info.cascade) {
			throw BinderException("Cannot drop vertex table %s of property graph %s: edge table %s references it. Drop "
			                      "the edge table first or use CASCADE",
			                      vertex_table->table_name, graph.property_graph_name, dependents[0]->table_name);
		}
		for (auto &edge_table : dependents) {
			RemoveTable(graph.edge_tables, edge_table);
		}
		RemoveTable(graph.vertex_tables, vertex_table);
	}
	if (graph.vertex_tables.empty()) {
		throw BinderException("Cannot drop every vertex table of property graph %s; use DROP PROPERTY GRAPH instead",
		                      graph.property_graph_name);
	}
}

void DropEdgeTables(ClientContext &context, AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph) {
	for (auto &edge_table : FindElementTables(context, info, graph, false)) {
		RemoveTable(graph.edge_tables, edge_table);
	}
}

//! In DuckPGQ the properties belong to the element table and are shared by all of its labels.
shared_ptr<PropertyGraphTable> FindLabeledTable(ClientContext &context, AlterPropertyGraphInfo &info,
                                                CreatePropertyGraphInfo &graph) {
	auto table = FindElementTable(context, graph, info.element_tables[0], info.alter_vertex_table);
	if (table->main_label == info.label) {
		return table;
	}
	for (auto &label : table->sub_labels) {
		if (label == info.label) {
			return table;
		}
	}
	throw BinderException("Label %s is not a label of %s table %s", info.label, ElementKind(info.alter_vertex_table),
	                      table->table_name);
}

void AddProperties(ClientContext &context, AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph) {
	auto table = FindLabeledTable(context, info, graph);
	auto entry = LookupTable(context, *table);
	if (!entry) {
		throw BinderException("Table %s does not exist", table->table_name);
	}
	for (idx_t i = 0; i < info.property_columns.size(); i++) {
		auto &column = info.property_columns[i];
		auto &property = info.property_names[i];
		if (!entry->ColumnExists(column)) {
			throw BinderException("Column %s not found in table %s", column, table->table_name);
		}
		for (idx_t j = 0; j < table->column_names.size(); j++) {
			// Only property names must be unique (11.26 SR 7); a column may be exposed under several names.
			if (table->column_aliases[j] == property) {
				throw BinderException("Property %s is already a property of label %s", property, info.label);
			}
		}
		table->column_names.push_back(column);
		table->column_aliases.push_back(property);
	}
}

void DropProperties(ClientContext &context, AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph) {
	auto table = FindLabeledTable(context, info, graph);
	CheckDistinct(info.property_names, "Property");
	for (auto &property : info.property_names) {
		idx_t index = 0;
		while (index < table->column_aliases.size() && table->column_aliases[index] != property) {
			index++;
		}
		if (index == table->column_aliases.size()) {
			throw BinderException("Property %s is not a property of label %s", property, info.label);
		}
		table->column_names.erase(table->column_names.begin() + static_cast<int64_t>(index));
		table->column_aliases.erase(table->column_aliases.begin() + static_cast<int64_t>(index));
	}
}

} // namespace

unique_ptr<FunctionData> AlterPropertyGraphFunction::AlterPropertyGraphBind(ClientContext &context,
                                                                            TableFunctionBindInput &,
                                                                            vector<LogicalType> &return_types,
                                                                            vector<Identifier> &names) {
	names.emplace_back("Success");
	return_types.emplace_back(LogicalType::BOOLEAN);
	auto duckpgq_state = GetDuckPGQState(context);
	auto duckpgq_parse_data = dynamic_cast<DuckPGQParseData *>(duckpgq_state->parse_data.get());
	if (!duckpgq_parse_data) {
		return {};
	}
	auto &statement = duckpgq_parse_data->statement->Cast<CreateStatement>();
	auto &info = statement.info->Cast<AlterPropertyGraphInfo>();
	auto &existing = *duckpgq_state->GetPropertyGraph(context, info.GetQualifiedName());

	auto result = make_uniq<AlterPropertyGraphBindData>();
	result->altered_graph = unique_ptr_cast<CreateInfo, CreatePropertyGraphInfo>(existing.Copy());
	auto &graph = *result->altered_graph;
	// Copied edge tables still point at vertex tables outside the copy (CreatePropertyGraphInfo::Copy keeps the
	// pointers), so point them at the copied vertex table with the same identity.
	auto relink = [&](shared_ptr<PropertyGraphTable> &vertex_table) {
		for (auto &table : graph.vertex_tables) {
			if (vertex_table && table->SameTableIdentity(*vertex_table)) {
				vertex_table = table;
				return;
			}
		}
		throw InternalException("Edge table of property graph %s references a vertex table outside the graph",
		                        graph.property_graph_name);
	};
	for (auto &edge_table : graph.edge_tables) {
		relink(edge_table->source_pg_table);
		relink(edge_table->destination_pg_table);
	}
	// The registered tables were validated when they were added; their property lists are already expanded.
	for (auto &table : graph.vertex_tables) {
		table->all_columns = false;
		table->no_columns = false;
		table->except_columns.clear();
	}
	for (auto &table : graph.edge_tables) {
		table->all_columns = false;
		table->no_columns = false;
		table->except_columns.clear();
	}

	switch (info.action) {
	case AlterPropertyGraphAction::ADD_TABLES:
		AddTables(context, info, graph, result->added_edge_tables);
		break;
	case AlterPropertyGraphAction::DROP_VERTEX_TABLES:
		DropVertexTables(context, info, graph);
		break;
	case AlterPropertyGraphAction::DROP_EDGE_TABLES:
		DropEdgeTables(context, info, graph);
		break;
	case AlterPropertyGraphAction::ADD_PROPERTIES:
		AddProperties(context, info, graph);
		break;
	case AlterPropertyGraphAction::DROP_PROPERTIES:
		DropProperties(context, info, graph);
		break;
	}
	RebuildLabelMap(graph);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> AlterPropertyGraphFunction::AlterPropertyGraphInit(ClientContext &,
                                                                                        TableFunctionInitInput &) {
	return make_uniq<AlterPropertyGraphGlobalData>();
}

void AlterPropertyGraphFunction::AlterPropertyGraphFunc(ClientContext &context, TableFunctionInput &data_p,
                                                        DataChunk &) {
	auto &bind_data = data_p.bind_data->Cast<AlterPropertyGraphBindData>();
	auto &graph = *bind_data.altered_graph;
	DuckPGQState::InitializeInternalTable(context);
	Connection connection(*context.db);
	// Added edge tables are checked like in CREATE; an incomplete edge fails the statement.
	auto &added = bind_data.added_edge_tables;
	bool verified = true;
	for (auto &edge_table : added) {
		verified &= CreatePropertyGraphFunction::VerifyEdgeTable(context, connection, edge_table);
	}
	// The other edge tables of a verified graph were checked when they were added. An unverified graph is checked as a
	// whole, so it becomes verified again once the edge tables that could not be checked are readable or dropped.
	if (!graph.verified) {
		for (auto &edge_table : graph.edge_tables) {
			if (std::find(added.begin(), added.end(), edge_table) != added.end()) {
				continue;
			}
			try {
				verified &= CreatePropertyGraphFunction::VerifyEdgeTable(context, connection, edge_table);
			} catch (std::exception &ex) {
				ErrorData error(ex);
				if (error.Type() != ExceptionType::CONSTRAINT && error.Type() != ExceptionType::INVALID) {
					throw;
				}
				// An existing edge table with an incomplete edge keeps the graph unverified; it does not fail ALTER.
				verified = false;
			}
		}
	}
	graph.verified = verified;
	CreatePropertyGraphFunction::StorePropertyGraph(context, connection, graph, true);
}

} // namespace duckdb

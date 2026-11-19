#include "duckpgq/core/functions/table/alter_property_graph.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
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

//! Element tables are named by their table name. Aliases are not persisted, so they cannot name an element table
//! reliably: another connection's ALTER, or a reload, replaces the definition with one that has none.
shared_ptr<PropertyGraphTable> FindElementTable(CreatePropertyGraphInfo &graph, const Identifier &name, bool vertex) {
	shared_ptr<PropertyGraphTable> result;
	for (auto &table : ElementTables(graph, vertex)) {
		if (table->table_name != name) {
			continue;
		}
		if (result) {
			throw BinderException("%s table name %s is ambiguous in property graph %s", ElementKind(vertex), name,
			                      graph.property_graph_name);
		}
		result = table;
	}
	if (!result) {
		throw BinderException("%s table %s is not part of property graph %s", ElementKind(vertex), name,
		                      graph.property_graph_name);
	}
	return result;
}

//! Names listed in a DROP list must be distinct.
void CheckDistinct(const vector<Identifier> &names, const char *what) {
	for (idx_t i = 0; i < names.size(); i++) {
		for (idx_t j = 0; j < i; j++) {
			if (names[i] == names[j]) {
				throw BinderException("%s %s is listed more than once", what, names[i]);
			}
		}
	}
}

//! The catalog entry a table reference resolves to, or nullptr. Qualifiers are bound like DuckDB does for tables, so
//! db2.person and memory.main.person resolve to the same entry as the bound names stored in the graph.
optional_ptr<TableCatalogEntry> ResolveTableEntry(ClientContext &context, Identifier catalog, Identifier schema,
                                                  const Identifier &name) {
	try {
		Binder::BindSchemaOrCatalog(context, catalog, schema);
		return Catalog::GetEntry<TableCatalogEntry>(context, QualifiedName(catalog, schema, name),
		                                            OnEntryNotFound::RETURN_NULL);
	} catch (Exception &) {
		return nullptr;
	}
}

//! Whether two element tables are over the same table, however their names were qualified.
bool SameTable(ClientContext &context, const PropertyGraphTable &left, const PropertyGraphTable &right) {
	auto left_entry = GetPropertyGraphTableEntry(context, left);
	auto right_entry = GetPropertyGraphTableEntry(context, right);
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
//! by the same statement (also by its alias) was linked by the transform; any other reference is resolved to its
//! catalog entry and matched against the vertex tables of the graph.
void ResolveEdgeReference(ClientContext &context, CreatePropertyGraphInfo &graph,
                          const unordered_map<PropertyGraphTable *, shared_ptr<PropertyGraphTable>> &added_vertices,
                          shared_ptr<PropertyGraphTable> &vertex_table, Identifier &catalog, Identifier &schema,
                          Identifier &reference) {
	if (vertex_table) {
		auto entry = added_vertices.find(vertex_table.get());
		vertex_table = entry == added_vertices.end() ? nullptr : entry->second;
	}
	if (!vertex_table) {
		auto entry = ResolveTableEntry(context, catalog, schema, reference);
		for (auto &table : graph.vertex_tables) {
			auto table_entry = GetPropertyGraphTableEntry(context, *table);
			if (entry ? table_entry.get() == entry.get()
			          : (catalog.empty() && schema.empty() && table->table_name == reference)) {
				vertex_table = table;
				break;
			}
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

void DropVertexTables(AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph) {
	CheckDistinct(info.element_tables, "Vertex table");
	for (auto &name : info.element_tables) {
		auto vertex_table = FindElementTable(graph, name, true);
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

void DropEdgeTables(AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph) {
	CheckDistinct(info.element_tables, "Edge table");
	for (auto &name : info.element_tables) {
		RemoveTable(graph.edge_tables, FindElementTable(graph, name, false));
	}
}

//! In DuckPGQ the properties belong to the element table and are shared by all of its labels.
shared_ptr<PropertyGraphTable> FindLabeledTable(AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph) {
	auto table = FindElementTable(graph, info.element_tables[0], info.alter_vertex_table);
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
	auto table = FindLabeledTable(info, graph);
	auto entry = GetPropertyGraphTableEntry(context, *table);
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

void DropProperties(AlterPropertyGraphInfo &info, CreatePropertyGraphInfo &graph) {
	auto table = FindLabeledTable(info, graph);
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
		DropVertexTables(info, graph);
		break;
	case AlterPropertyGraphAction::DROP_EDGE_TABLES:
		DropEdgeTables(info, graph);
		break;
	case AlterPropertyGraphAction::ADD_PROPERTIES:
		AddProperties(context, info, graph);
		break;
	case AlterPropertyGraphAction::DROP_PROPERTIES:
		DropProperties(info, graph);
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
	// Edge tables already in the graph were checked when they were added; a graph that was not verified stays so.
	for (auto &edge_table : bind_data.added_edge_tables) {
		graph.verified &= CreatePropertyGraphFunction::VerifyEdgeTable(context, connection, edge_table);
	}
	CreatePropertyGraphFunction::StorePropertyGraph(context, connection, graph, true);
}

} // namespace duckdb

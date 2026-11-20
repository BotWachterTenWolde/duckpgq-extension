#include "duckpgq/core/parser/duckpgq_grammar.hpp"
#include "duckpgq/core/parser/duckpgq_parser.hpp"

#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/extension_statement.hpp"
#include "duckpgq/parser/parsed_data/alter_property_graph_info.hpp"
#include "duckpgq_create_property_graph_grammar.hpp"

namespace duckdb {

namespace {

template <class T>
static vector<T> TransformParensList(PEGTransformer &transformer, ParseResult &parse_result) {
	vector<T> result;
	auto &list = PEGTransformerFactory::ExtractResultFromParens(parse_result);
	for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(list)) {
		result.push_back(transformer.Transform<T>(item.get()));
	}
	return result;
}

//! Parens(List(DuckPGQProperty)): each property is a column, exposed under its alias or else its own name.
static void TransformPropertyList(PEGTransformer &transformer, ParseResult &parse_result, vector<Identifier> &columns,
                                  vector<Identifier> &aliases) {
	auto &items = PEGTransformerFactory::ExtractResultFromParens(parse_result);
	for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(items)) {
		auto &property = item.get().Cast<ListParseResult>();
		auto column = transformer.Transform<Identifier>(property.GetChild(0));
		auto &alias = property.Child<OptionalParseResult>(1);
		columns.push_back(column);
		aliases.push_back(alias.HasResult()
		                      ? transformer.Transform<Identifier>(alias.GetResult().Cast<ListParseResult>().GetChild(1))
		                      : column);
	}
}

static void ApplyProperties(PEGTransformer &transformer, OptionalParseResult &properties, PropertyGraphTable &table) {
	if (!properties.HasResult()) {
		table.all_columns = true;
		return;
	}
	auto &choice = DuckPGQGetChoice(properties.GetResult());
	auto &list = choice.Cast<ListParseResult>();
	if (choice.Name() == "DuckPGQNoProperties") {
		table.no_columns = true;
	} else if (choice.Name() == "DuckPGQPropertyList") {
		TransformPropertyList(transformer, list.GetChild(1), table.column_names, table.column_aliases);
	} else {
		table.all_columns = true;
		if (choice.Name() == "DuckPGQAllColumnsExcept") {
			table.except_columns = TransformParensList<Identifier>(transformer, list.GetChild(5));
		}
	}
}

static void ApplySubLabels(PEGTransformer &transformer, ParseResult &parse_result, PropertyGraphTable &table) {
	auto &list = parse_result.Cast<ListParseResult>();
	table.discriminator = transformer.Transform<Identifier>(list.GetChild(1));
	table.sub_labels = TransformParensList<Identifier>(transformer, list.GetChild(2));
}

static void ApplyLabel(PEGTransformer &transformer, OptionalParseResult &label, PropertyGraphTable &table) {
	table.main_label = table.table_name_alias.empty() ? table.table_name : table.table_name_alias;
	if (!label.HasResult()) {
		return;
	}
	auto &choice = DuckPGQGetChoice(label.GetResult());
	auto &list = choice.Cast<ListParseResult>();
	if (choice.Name() == "DuckPGQExplicitLabel") {
		table.main_label = transformer.Transform<Identifier>(list.GetChild(1));
		auto &sub_labels = list.Child<OptionalParseResult>(2);
		if (sub_labels.HasResult()) {
			ApplySubLabels(transformer, sub_labels.GetResult(), table);
		}
	} else {
		ApplySubLabels(transformer, list.GetChild(0), table);
	}
}

static void ApplyReference(PEGTransformer &transformer, ParseResult &parse_result, PropertyGraphTable &edge,
                           bool source) {
	auto &reference = DuckPGQGetChoice(parse_result.Cast<ListParseResult>().GetChild(1));
	unique_ptr<BaseTableRef> base_table;
	auto &foreign_keys = source ? edge.source_fk : edge.destination_fk;
	auto &primary_keys = source ? edge.source_pk : edge.destination_pk;
	if (reference.Name() == "DuckPGQKeyReference") {
		auto &list = reference.Cast<ListParseResult>();
		foreign_keys = TransformParensList<Identifier>(transformer, list.GetChild(1));
		base_table = transformer.Transform<unique_ptr<BaseTableRef>>(list.GetChild(3));
		primary_keys = TransformParensList<Identifier>(transformer, list.GetChild(4));
	} else {
		base_table = transformer.Transform<unique_ptr<BaseTableRef>>(reference);
	}
	auto &name = base_table->GetQualifiedName();
	(source ? edge.source_catalog : edge.destination_catalog) = name.Catalog();
	(source ? edge.source_schema : edge.destination_schema) = name.Schema();
	(source ? edge.source_reference : edge.destination_reference) = name.Name();
}

// Called directly rather than through a registered transform: an extension-defined transform result type would
// instantiate the dllimport-declared TypedTransformResult template, which does not link on Windows.
static shared_ptr<PropertyGraphTable> TransformGraphTable(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto table = make_shared_ptr<PropertyGraphTable>();
	table->is_vertex_table = parse_result.Name() == "DuckPGQVertexTable";
	auto base_table = transformer.Transform<unique_ptr<BaseTableRef>>(list.GetChild(0));
	auto &name = base_table->GetQualifiedName();
	table->catalog_name = name.Catalog();
	table->schema_name = name.Schema();
	table->table_name = name.Name();
	auto &alias = list.Child<OptionalParseResult>(1);
	if (alias.HasResult()) {
		table->table_name_alias = transformer.Transform<TableAlias>(alias.GetResult()).name;
	}
	idx_t properties_index = 2;
	if (!table->is_vertex_table) {
		ApplyReference(transformer, list.GetChild(2), *table, true);
		ApplyReference(transformer, list.GetChild(3), *table, false);
		properties_index = 4;
	}
	ApplyProperties(transformer, list.Child<OptionalParseResult>(properties_index), *table);
	ApplyLabel(transformer, list.Child<OptionalParseResult>(properties_index + 1), *table);
	return table;
}

static vector<shared_ptr<PropertyGraphTable>> TransformGraphTables(PEGTransformer &transformer,
                                                                   ParseResult &parse_result) {
	vector<shared_ptr<PropertyGraphTable>> result;
	auto &list = PEGTransformerFactory::ExtractResultFromParens(parse_result);
	for (auto &item : PEGTransformerFactory::ExtractParseResultsFromList(list)) {
		result.push_back(TransformGraphTable(transformer, item.get()));
	}
	return result;
}

static shared_ptr<PropertyGraphTable> FindVertexTable(const CreatePropertyGraphInfo &info, const Identifier &catalog,
                                                      const Identifier &schema, const Identifier &name) {
	for (auto &table : info.vertex_tables) {
		if ((!catalog.empty() && table->catalog_name != catalog) || (!schema.empty() && table->schema_name != schema)) {
			continue;
		}
		if (table->table_name == name || (!table->table_name_alias.empty() && table->table_name_alias == name)) {
			return table;
		}
	}
	return nullptr;
}

static void ResolveReference(const shared_ptr<PropertyGraphTable> &vertex, Identifier &catalog, Identifier &schema,
                             Identifier &reference) {
	if (!vertex) {
		return; // left as written so the binder can report the unknown reference
	}
	catalog = vertex->catalog_name;
	schema = vertex->schema_name;
	reference = vertex->table_name;
}

static void RegisterLabels(CreatePropertyGraphInfo &info, const shared_ptr<PropertyGraphTable> &table) {
	auto register_label = [&](const Identifier &label) {
		auto name = label.GetIdentifierName();
		if (!info.label_map.emplace(name, table).second) {
			throw ConstraintException("Label %s is not unique, make sure all labels are unique",
			                          StringUtil::Lower(name));
		}
	};
	register_label(table->main_label);
	for (auto &label : table->sub_labels) {
		register_label(label);
	}
}

//! Registers the labels of the statement's element tables and links each edge table to the statement's vertex tables.
static void ResolveStatementTables(CreatePropertyGraphInfo &info) {
	for (auto &table : info.vertex_tables) {
		RegisterLabels(info, table);
	}
	for (auto &table : info.edge_tables) {
		table->source_pg_table =
		    FindVertexTable(info, table->source_catalog, table->source_schema, table->source_reference);
		table->destination_pg_table =
		    FindVertexTable(info, table->destination_catalog, table->destination_schema, table->destination_reference);
		// A reference may use the vertex table's statement alias; the rest of the extension compares physical names.
		ResolveReference(table->source_pg_table, table->source_catalog, table->source_schema, table->source_reference);
		ResolveReference(table->destination_pg_table, table->destination_catalog, table->destination_schema,
		                 table->destination_reference);
		RegisterLabels(info, table);
	}
}

//! Wraps the info in a CreateStatement inside an ExtensionStatement, planned by the parser extension.
static arena_ptr<TransformResultValue> MakeGraphStatement(PEGTransformer &transformer,
                                                          unique_ptr<CreatePropertyGraphInfo> info) {
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto parse_data = make_uniq_base<ParserExtensionParseData, DuckPGQParseData>(std::move(statement));
	unique_ptr<SQLStatement> result = make_uniq<ExtensionStatement>(DuckPGQParserExtension(), std::move(parse_data));
	return transformer.MakeResult<unique_ptr<SQLStatement>>(std::move(result));
}

static arena_ptr<TransformResultValue> TransformCreateGraph(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto or_replace = list.Child<OptionalParseResult>(1).HasResult();
	auto if_not_exists = list.Child<OptionalParseResult>(4).HasResult();
	if (or_replace && if_not_exists) {
		throw ParserException("Cannot specify both OR REPLACE and IF NOT EXISTS within single create statement");
	}
	auto name = transformer.Transform<QualifiedName>(list.GetChild(5));
	if (name.Name().empty()) {
		throw ParserException("Empty property graph name not supported");
	}
	auto info = make_uniq<CreatePropertyGraphInfo>(name.Name().GetIdentifierName());
	info->SetQualifiedName(name);
	info->on_conflict = or_replace      ? OnCreateConflict::REPLACE_ON_CONFLICT
	                    : if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT
	                                    : OnCreateConflict::ERROR_ON_CONFLICT;
	info->vertex_tables = TransformGraphTables(transformer, list.GetChild(6).Cast<ListParseResult>().GetChild(2));
	auto &edges = list.Child<OptionalParseResult>(7);
	if (edges.HasResult()) {
		info->edge_tables = TransformGraphTables(transformer, edges.GetResult().Cast<ListParseResult>().GetChild(2));
	}
	ResolveStatementTables(*info);
	return MakeGraphStatement(transformer, std::move(info));
}

static arena_ptr<TransformProcess> StartCreateGraphTransform(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, TransformCreateGraph);
}

static bool IsCascade(OptionalParseResult &drop_behavior) {
	return drop_behavior.HasResult() && DuckPGQLiteralText(drop_behavior.GetResult()) == "CASCADE";
}

static bool IsVertexKind(ParseResult &element_kind) {
	return DuckPGQLiteralText(element_kind) == "VERTEX";
}

//! A BaseTableName as written: [catalog.][schema.]name.
static QualifiedName TransformTableName(PEGTransformer &transformer, ParseResult &parse_result) {
	auto base_table = transformer.Transform<unique_ptr<BaseTableRef>>(parse_result);
	return base_table->GetQualifiedName();
}

// 'ALTER' 'PROPERTY' 'GRAPH' QualifiedName DuckPGQAlterGraphAction
static arena_ptr<TransformResultValue> TransformAlterGraph(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto name = transformer.Transform<QualifiedName>(list.GetChild(3));
	auto info = make_uniq<AlterPropertyGraphInfo>(name.Name().GetIdentifierName());
	info->SetQualifiedName(name);
	auto &action = DuckPGQGetChoice(list.GetChild(4));
	auto &action_list = action.Cast<ListParseResult>();
	if (action.Name() == "DuckPGQAddVertexTables" || action.Name() == "DuckPGQAddEdgeTables") {
		// 'ADD' DuckPGQVertexTables DuckPGQAddEdgeTables? / 'ADD' DuckPGQEdgeTables
		info->action = AlterPropertyGraphAction::ADD_TABLES;
		optional_ptr<ParseResult> edges;
		if (action.Name() == "DuckPGQAddVertexTables") {
			info->vertex_tables =
			    TransformGraphTables(transformer, action_list.GetChild(1).Cast<ListParseResult>().GetChild(2));
			auto &add_edges = action_list.Child<OptionalParseResult>(2);
			if (add_edges.HasResult()) {
				edges = add_edges.GetResult().Cast<ListParseResult>().GetChild(1);
			}
		} else {
			edges = action_list.GetChild(1);
		}
		if (edges) {
			info->edge_tables = TransformGraphTables(transformer, edges->Cast<ListParseResult>().GetChild(2));
		}
		// References to vertex tables already in the graph stay unresolved until the statement is bound.
		ResolveStatementTables(*info);
	} else if (action.Name() == "DuckPGQDropElementTables") {
		// 'DROP' DuckPGQElementKind 'TABLES' Parens(List(BaseTableName)) DuckPGQDropBehavior?
		info->action = IsVertexKind(action_list.GetChild(1)) ? AlterPropertyGraphAction::DROP_VERTEX_TABLES
		                                                     : AlterPropertyGraphAction::DROP_EDGE_TABLES;
		auto &tables = PEGTransformerFactory::ExtractResultFromParens(action_list.GetChild(3));
		for (auto &table : PEGTransformerFactory::ExtractParseResultsFromList(tables)) {
			info->element_tables.push_back(TransformTableName(transformer, table.get()));
		}
		info->cascade = IsCascade(action_list.Child<OptionalParseResult>(4));
	} else {
		// 'ALTER' DuckPGQElementKind 'TABLE' BaseTableName 'ALTER' 'LABEL' ColId DuckPGQAlterLabelAction
		info->alter_vertex_table = IsVertexKind(action_list.GetChild(1));
		info->element_tables.push_back(TransformTableName(transformer, action_list.GetChild(3)));
		info->label = transformer.Transform<Identifier>(action_list.GetChild(6));
		auto &label_action = DuckPGQGetChoice(action_list.GetChild(7));
		auto &label_list = label_action.Cast<ListParseResult>();
		if (label_action.Name() == "DuckPGQAddLabelProperties") {
			// 'ADD' 'PROPERTIES' Parens(List(DuckPGQProperty))
			info->action = AlterPropertyGraphAction::ADD_PROPERTIES;
			TransformPropertyList(transformer, label_list.GetChild(2), info->property_columns, info->property_names);
		} else {
			// 'DROP' 'PROPERTIES' Parens(List(ColId)) DuckPGQDropBehavior?
			info->action = AlterPropertyGraphAction::DROP_PROPERTIES;
			info->property_names = TransformParensList<Identifier>(transformer, label_list.GetChild(2));
			// Dependencies on properties are not tracked, so CASCADE and RESTRICT behave the same.
			info->cascade = IsCascade(label_list.Child<OptionalParseResult>(3));
		}
	}
	return MakeGraphStatement(transformer, std::move(info));
}

static arena_ptr<TransformProcess> StartAlterGraphTransform(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, TransformAlterGraph);
}

} // namespace

void DuckPGQAddCreatePropertyGraphGrammar(vector<GrammarChange> &changes) {
	// Grammar literals that are not DuckDB keywords match by text, so no keywords need registering.
	for (auto rule : DUCKPGQ_CREATE_PROPERTY_GRAPH_RULES) {
		changes.push_back(GrammarChange::AddRule(rule));
	}
	changes.push_back(GrammarChange::SetTransformProcess("DuckPGQCreatePropertyGraph", StartCreateGraphTransform));
	// Return an ExtensionStatement directly, without intercepting ordinary CREATE statements.
	changes.push_back(GrammarChange::PrependChoice("Statement", "DuckPGQCreatePropertyGraph"));
	changes.push_back(GrammarChange::SetTransformProcess("DuckPGQAlterPropertyGraph", StartAlterGraphTransform));
	changes.push_back(GrammarChange::PrependChoice("Statement", "DuckPGQAlterPropertyGraph"));
}

} // namespace duckdb

//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/parser/parsed_data/alter_property_graph_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckpgq/parser/parsed_data/create_property_graph_info.hpp"

namespace duckdb {

enum class AlterPropertyGraphAction : uint8_t {
	//! ADD VERTEX TABLES (...) [ADD EDGE TABLES (...)] or ADD EDGE TABLES (...)
	ADD_TABLES,
	//! DROP VERTEX TABLES (...) [CASCADE | RESTRICT]
	DROP_VERTEX_TABLES,
	//! DROP EDGE TABLES (...) [CASCADE | RESTRICT]
	DROP_EDGE_TABLES,
	//! ALTER {VERTEX | EDGE} TABLE t ALTER LABEL l ADD PROPERTIES (...)
	ADD_PROPERTIES,
	//! ALTER {VERTEX | EDGE} TABLE t ALTER LABEL l DROP PROPERTIES (...) [CASCADE | RESTRICT]
	DROP_PROPERTIES,
};

//! ALTER PROPERTY GRAPH (SQL/PGQ, ISO/IEC 9075-16, 11.20). The inherited vertex_tables and edge_tables hold the element
//! tables an ADD action adds; the graph being altered is looked up when the statement is bound. It derives from
//! CreatePropertyGraphInfo so it can travel in a CreateStatement like CREATE PROPERTY GRAPH does.
struct AlterPropertyGraphInfo : public CreatePropertyGraphInfo {
	explicit AlterPropertyGraphInfo(string property_graph_name)
	    : CreatePropertyGraphInfo(std::move(property_graph_name)) {
	}

	AlterPropertyGraphAction action = AlterPropertyGraphAction::ADD_TABLES;
	//! DROP ... TABLES: the element tables to drop. ALTER ... TABLE: the one element table to alter.
	vector<Identifier> element_tables;
	//! ALTER {VERTEX | EDGE} TABLE: whether a vertex table is altered.
	bool alter_vertex_table = true;
	//! ALTER LABEL: the label whose properties change.
	Identifier label;
	//! ADD PROPERTIES: the columns and the property names they are exposed as. DROP PROPERTIES: the property names.
	vector<Identifier> property_columns;
	vector<Identifier> property_names;
	//! CASCADE rather than RESTRICT (the default).
	bool cascade = false;

	unique_ptr<CreateInfo> Copy() const override {
		auto result = make_uniq<AlterPropertyGraphInfo>(property_graph_name);
		CopyProperties(*result);
		for (auto &table : vertex_tables) {
			result->vertex_tables.push_back(table->Copy());
		}
		for (auto &table : edge_tables) {
			result->edge_tables.push_back(table->Copy());
		}
		result->action = action;
		result->element_tables = element_tables;
		result->alter_vertex_table = alter_vertex_table;
		result->label = label;
		result->property_columns = property_columns;
		result->property_names = property_names;
		result->cascade = cascade;
		return std::move(result);
	}
};

} // namespace duckdb

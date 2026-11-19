//===----------------------------------------------------------------------===//
//                         DuckPGQ
//
// duckpgq/core/functions/table/alter_property_graph.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "duckpgq/common.hpp"
#include "duckdb/function/table_function.hpp"
#include <duckpgq/parser/parsed_data/alter_property_graph_info.hpp>

namespace duckdb {

//! Executes ALTER PROPERTY GRAPH. The bind builds the altered definition from the registered graph and validates the
//! element tables it adds or changes; the execution replaces the stored definition, like CREATE OR REPLACE does.
class AlterPropertyGraphFunction : public TableFunction {
public:
	AlterPropertyGraphFunction() {
		name = "alter_property_graph";
		bind = AlterPropertyGraphBind;
		init_global = AlterPropertyGraphInit;
		function = AlterPropertyGraphFunc;
	}

	struct AlterPropertyGraphBindData : public TableFunctionData {
		//! The graph as it is after the alteration.
		unique_ptr<CreatePropertyGraphInfo> altered_graph;
		//! Edge tables the alteration adds; only these are checked for incomplete edges.
		vector<shared_ptr<PropertyGraphTable>> added_edge_tables;
	};

	struct AlterPropertyGraphGlobalData : public GlobalTableFunctionState {
		AlterPropertyGraphGlobalData() = default;
	};

	static unique_ptr<FunctionData> AlterPropertyGraphBind(ClientContext &context, TableFunctionBindInput &input,
	                                                       vector<LogicalType> &return_types,
	                                                       vector<Identifier> &names);

	static unique_ptr<GlobalTableFunctionState> AlterPropertyGraphInit(ClientContext &context,
	                                                                   TableFunctionInitInput &input);

	static void AlterPropertyGraphFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
};

} // namespace duckdb

#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/TreeRewriter.h>
#include <Storages/ProjectionsDescription.h>
#include <Storages/MergeTree/MergeTreeVirtualColumns.h>
#include <Storages/StorageInMemoryMetadata.h>

#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTProjectionDeclaration.h>
#include <Parsers/ASTProjectionSelectQuery.h>
#include <Parsers/ParserCreateQuery.h>
#include <Parsers/parseQuery.h>

#include <Columns/ColumnConst.h>
#include <Core/Defines.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/ExpressionActions.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <Processors/Transforms/PlanSquashingTransform.h>
#include <Processors/Transforms/SquashingTransform.h>
#include <QueryPipeline/Pipe.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <base/range.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int INCORRECT_QUERY;
    extern const int NO_SUCH_PROJECTION_IN_TABLE;
    extern const int ILLEGAL_PROJECTION;
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
}

bool ProjectionDescription::isPrimaryKeyColumnPossiblyWrappedInFunctions(const ASTPtr & node) const
{
    const String column_name = node->getColumnName();

    for (const auto & key_name : metadata->getPrimaryKeyColumns())
        if (column_name == key_name)
            return true;

    if (const auto * func = node->as<ASTFunction>())
        if (func->arguments->children.size() == 1)
            return isPrimaryKeyColumnPossiblyWrappedInFunctions(func->arguments->children.front());

    return false;
}


ProjectionDescription ProjectionDescription::clone() const
{
    ProjectionDescription other;
    if (definition_ast)
        other.definition_ast = definition_ast->clone();
    if (query_ast)
        other.query_ast = query_ast->clone();

    other.name = name;
    other.type = type;
    other.required_columns = required_columns;
    other.sample_block = sample_block;
    other.sample_block_for_keys = sample_block_for_keys;
    other.metadata = metadata;
    other.key_size = key_size;
    other.primary_key_max_column_name = primary_key_max_column_name;
    other.partition_value_indices = partition_value_indices;

    return other;
}

ProjectionsDescription ProjectionsDescription::clone() const
{
    ProjectionsDescription other;
    for (const auto & projection : projections)
        other.add(projection.clone());

    return other;
}

bool ProjectionDescription::operator==(const ProjectionDescription & other) const
{
    return name == other.name && definition_ast->formatWithSecretsOneLine() == other.definition_ast->formatWithSecretsOneLine();
}

ProjectionDescription
ProjectionDescription::getProjectionFromAST(const ASTPtr & definition_ast, const ColumnsDescription & columns, ContextPtr query_context)
{
    const auto * projection_definition = definition_ast->as<ASTProjectionDeclaration>();

    if (!projection_definition)
        throw Exception(ErrorCodes::INCORRECT_QUERY, "Cannot create projection from non ASTProjectionDeclaration AST");

    if (projection_definition->name.empty())
        throw Exception(ErrorCodes::INCORRECT_QUERY, "Projection must have name in definition.");

    if (!projection_definition->query)
        throw Exception(ErrorCodes::INCORRECT_QUERY, "QUERY is required for projection");

    ProjectionDescription result;
    result.definition_ast = projection_definition->clone();
    result.name = projection_definition->name;

    auto query = projection_definition->query->as<ASTProjectionSelectQuery &>();
    result.query_ast = query.cloneToASTSelect();

    auto external_storage_holder = std::make_shared<TemporaryTableHolder>(query_context, columns, ConstraintsDescription{});
    StoragePtr storage = external_storage_holder->getTable();
    InterpreterSelectQuery select(
        result.query_ast,
        query_context,
        storage,
        {},
        /// Here we ignore ast optimizations because otherwise aggregation keys may be removed from result header as constants.
        SelectQueryOptions{QueryProcessingStage::WithMergeableState}
            .modify()
            .ignoreAlias()
            .ignoreASTOptimizations()
            .ignoreSettingConstraints());

    result.required_columns = select.getRequiredColumns();
    result.sample_block = select.getSampleBlock();

    StorageInMemoryMetadata metadata;
    metadata.partition_key = KeyDescription::buildEmptyKey();

    const auto & query_select = result.query_ast->as<const ASTSelectQuery &>();
    if (select.hasAggregation())
    {
        if (query.orderBy())
            throw Exception(ErrorCodes::ILLEGAL_PROJECTION, "When aggregation is used in projection, ORDER BY cannot be specified");

        result.type = ProjectionDescription::Type::Aggregate;
        if (const auto & group_expression_list = query_select.groupBy())
        {
            ASTPtr order_expression;
            if (group_expression_list->children.size() == 1)
            {
                result.key_size = 1;
                order_expression = std::make_shared<ASTIdentifier>(group_expression_list->children.front()->getColumnName());
            }
            else
            {
                auto function_node = std::make_shared<ASTFunction>();
                function_node->name = "tuple";
                function_node->arguments = group_expression_list->clone();
                result.key_size = function_node->arguments->children.size();
                for (auto & child : function_node->arguments->children)
                    child = std::make_shared<ASTIdentifier>(child->getColumnName());
                function_node->children.push_back(function_node->arguments);
                order_expression = function_node;
            }
            auto columns_with_state = ColumnsDescription(result.sample_block.getNamesAndTypesList());
            metadata.sorting_key = KeyDescription::getSortingKeyFromAST(order_expression, columns_with_state, query_context, {});
            metadata.primary_key = KeyDescription::getKeyFromAST(order_expression, columns_with_state, query_context);
            metadata.primary_key.definition_ast = nullptr;
        }
        else
        {
            metadata.sorting_key = KeyDescription::buildEmptyKey();
            metadata.primary_key = KeyDescription::buildEmptyKey();
        }
        for (const auto & key : select.getQueryAnalyzer()->aggregationKeys())
            result.sample_block_for_keys.insert({nullptr, key.type, key.name});
    }
    else
    {
        result.type = ProjectionDescription::Type::Normal;
        metadata.sorting_key = KeyDescription::getSortingKeyFromAST(query.orderBy(), columns, query_context, {});
        metadata.primary_key = KeyDescription::getKeyFromAST(query.orderBy(), columns, query_context);
        metadata.primary_key.definition_ast = nullptr;
    }

    auto block = result.sample_block;
    for (const auto & [name, type] : metadata.sorting_key.expression->getRequiredColumnsWithTypes())
        block.insertUnique({nullptr, type, name});
    for (const auto & column_with_type_name : block)
    {
        if (column_with_type_name.column && isColumnConst(*column_with_type_name.column))
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Projections cannot contain constant columns: {}", column_with_type_name.name);
    }

    metadata.setColumns(ColumnsDescription(block.getNamesAndTypesList()));
    result.metadata = std::make_shared<StorageInMemoryMetadata>(metadata);
    return result;
}

ProjectionDescription ProjectionDescription::getMinMaxCountProjection(
    const ColumnsDescription & columns,
    ASTPtr partition_columns,
    const Names & minmax_columns,
    const ASTs & primary_key_asts,
    ContextPtr query_context)
{
    ProjectionDescription result;

    auto select_query = std::make_shared<ASTProjectionSelectQuery>();
    ASTPtr select_expression_list = std::make_shared<ASTExpressionList>();
    for (const auto & column : minmax_columns)
    {
        select_expression_list->children.push_back(makeASTFunction("min", std::make_shared<ASTIdentifier>(column)));
        select_expression_list->children.push_back(makeASTFunction("max", std::make_shared<ASTIdentifier>(column)));
    }
    if (!primary_key_asts.empty())
    {
        select_expression_list->children.push_back(makeASTFunction("min", primary_key_asts.front()->clone()));
        select_expression_list->children.push_back(makeASTFunction("max", primary_key_asts.front()->clone()));
    }
    select_expression_list->children.push_back(makeASTFunction("count"));
    select_query->setExpression(ASTProjectionSelectQuery::Expression::SELECT, std::move(select_expression_list));

    if (partition_columns && !partition_columns->children.empty())
    {
        partition_columns = partition_columns->clone();
        for (const auto & partition_column : partition_columns->children)
            KeyDescription::moduloToModuloLegacyRecursive(partition_column);
        select_query->setExpression(ASTProjectionSelectQuery::Expression::GROUP_BY, partition_columns->clone());
    }

    result.definition_ast = select_query;
    result.name = MINMAX_COUNT_PROJECTION_NAME;
    result.query_ast = select_query->cloneToASTSelect();

    auto external_storage_holder = std::make_shared<TemporaryTableHolder>(query_context, columns, ConstraintsDescription{});
    StoragePtr storage = external_storage_holder->getTable();
    InterpreterSelectQuery select(
        result.query_ast,
        query_context,
        storage,
        {},
        /// Here we ignore ast optimizations because otherwise aggregation keys may be removed from result header as constants.
        SelectQueryOptions{QueryProcessingStage::WithMergeableState}
            .modify()
            .ignoreAlias()
            .ignoreASTOptimizations()
            .ignoreSettingConstraints());
    result.required_columns = select.getRequiredColumns();
    result.sample_block = select.getSampleBlock();

    std::set<size_t> constant_positions;
    for (size_t i = 0; i < result.sample_block.columns(); ++i)
    {
        if (typeid_cast<const ColumnConst *>(result.sample_block.getByPosition(i).column.get()))
            constant_positions.insert(i);
    }
    result.sample_block.erase(constant_positions);

    const auto & analysis_result = select.getAnalysisResult();
    if (analysis_result.need_aggregate)
    {
        for (const auto & key : select.getQueryAnalyzer()->aggregationKeys())
        {
            if (result.sample_block.has(key.name))
            {
                result.sample_block_for_keys.insert({nullptr, key.type, key.name});
                result.partition_value_indices.push_back(result.sample_block.getPositionByName(key.name));
            }
        }
    }

    /// If we have primary key and it's not in minmax_columns, it will be used as one additional minmax columns.
    if (!primary_key_asts.empty()
        && result.sample_block.columns()
            == 2 * (minmax_columns.size() + 1) /* minmax columns */ + 1 /* count() */
                + result.partition_value_indices.size() /* partition_columns */)
    {
        /// partition_expr1, partition_expr2, ..., min(p1), max(p1), min(p2), max(p2), ..., min(k1), max(k1), count()
        ///                                                                                              ^
        ///                                                                                           size - 2
        result.primary_key_max_column_name = *(result.sample_block.getNames().cend() - 2);
    }
    result.type = ProjectionDescription::Type::Aggregate;
    StorageInMemoryMetadata metadata;
    metadata.setColumns(ColumnsDescription(result.sample_block.getNamesAndTypesList()));
    metadata.partition_key = KeyDescription::buildEmptyKey();
    metadata.sorting_key = KeyDescription::buildEmptyKey();
    metadata.primary_key = KeyDescription::buildEmptyKey();
    result.metadata = std::make_shared<StorageInMemoryMetadata>(metadata);
    return result;
}

void ProjectionDescription::recalculateWithNewColumns(const ColumnsDescription & new_columns, ContextPtr query_context)
{
    *this = getProjectionFromAST(definition_ast, new_columns, query_context);
}

Block ProjectionDescription::calculate(const Block & block, ContextPtr context) const
{
    auto mut_context = Context::createCopy(context);
    /// We ignore aggregate_functions_null_for_empty cause it changes aggregate function types.
    /// Now, projections do not support in on SELECT, and (with this change) should ignore on INSERT as well.
    mut_context->setSetting("aggregate_functions_null_for_empty", Field(0));
    mut_context->setSetting("transform_null_in", Field(0));

    ASTPtr query_ast_copy = nullptr;
    /// Respect the _row_exists column.
    if (block.has(RowExistsColumn::name))
    {
        query_ast_copy = query_ast->clone();
        auto * select_row_exists = query_ast_copy->as<ASTSelectQuery>();
        if (!select_row_exists)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot get ASTSelectQuery when adding _row_exists = 1. It's a bug");

        select_row_exists->setExpression(
            ASTSelectQuery::Expression::WHERE,
            makeASTFunction("equals", std::make_shared<ASTIdentifier>(RowExistsColumn::name), std::make_shared<ASTLiteral>(1)));
    }

    auto builder = InterpreterSelectQuery(
                       query_ast_copy ? query_ast_copy : query_ast,
                       mut_context,
                       Pipe(std::make_shared<SourceFromSingleChunk>(block)),
                       SelectQueryOptions{
                           type == ProjectionDescription::Type::Normal ? QueryProcessingStage::FetchColumns
                                                                       : QueryProcessingStage::WithMergeableState}
                           .ignoreASTOptimizations()
                           .ignoreSettingConstraints())
                       .buildQueryPipeline();
    builder.resize(1);
    // Generate aggregated blocks with rows less or equal than the original block.
    // There should be only one output block after this transformation.

    builder.addTransform(std::make_shared<PlanSquashingTransform>(builder.getHeader(), block.rows(), 0));
    builder.addTransform(std::make_shared<ApplySquashingTransform>(builder.getHeader(), block.rows(), 0));

    auto pipeline = QueryPipelineBuilder::getPipeline(std::move(builder));
    PullingPipelineExecutor executor(pipeline);
    Block ret;
    executor.pull(ret);
    if (executor.pull(ret))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Projection cannot increase the number of rows in a block. It's a bug");
    return ret;
}


String ProjectionsDescription::toString() const
{
    if (empty())
        return {};

    ASTExpressionList list;
    for (const auto & projection : projections)
        list.children.push_back(projection.definition_ast);

    return list.formatWithSecretsOneLine();
}

ProjectionsDescription ProjectionsDescription::parse(const String & str, const ColumnsDescription & columns, ContextPtr query_context)
{
    ProjectionsDescription result;
    if (str.empty())
        return result;

    ParserProjectionDeclarationList parser;
    ASTPtr list = parseQuery(parser, str, 0, DBMS_DEFAULT_MAX_PARSER_DEPTH, DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);

    for (const auto & projection_ast : list->children)
    {
        auto projection = ProjectionDescription::getProjectionFromAST(projection_ast, columns, query_context);
        result.add(std::move(projection));
    }

    return result;
}

bool ProjectionsDescription::has(const String & projection_name) const
{
    return map.contains(projection_name);
}

const ProjectionDescription & ProjectionsDescription::get(const String & projection_name) const
{
    auto it = map.find(projection_name);
    if (it == map.end())
    {
        throw Exception(ErrorCodes::NO_SUCH_PROJECTION_IN_TABLE, "There is no projection {} in table{}",
                        projection_name, getHintsMessage(projection_name));
    }

    return *(it->second);
}

void ProjectionsDescription::add(ProjectionDescription && projection, const String & after_projection, bool first, bool if_not_exists)
{
    if (has(projection.name))
    {
        if (if_not_exists)
            return;
        throw Exception(
            ErrorCodes::ILLEGAL_PROJECTION, "Cannot add projection {}: projection with this name already exists", projection.name);
    }

    auto insert_it = projections.cend();

    if (first)
        insert_it = projections.cbegin();
    else if (!after_projection.empty())
    {
        auto it = std::find_if(
            projections.cbegin(),
            projections.cend(),
            [&after_projection](const auto & projection_) { return projection_.name == after_projection; });
        if (it != projections.cend())
            ++it;
        insert_it = it;
    }

    auto it = projections.insert(insert_it, std::move(projection));
    map[it->name] = it;
}

void ProjectionsDescription::remove(const String & projection_name, bool if_exists)
{
    auto it = map.find(projection_name);
    if (it == map.end())
    {
        if (if_exists)
            return;

        throw Exception(ErrorCodes::NO_SUCH_PROJECTION_IN_TABLE, "There is no projection {} in table{}",
                        projection_name, getHintsMessage(projection_name));
    }

    projections.erase(it->second);
    map.erase(it);
}

std::vector<String> ProjectionsDescription::getAllRegisteredNames() const
{
    std::vector<String> names;
    names.reserve(map.size());
    for (const auto & pair : map)
        names.push_back(pair.first);
    return names;
}

ExpressionActionsPtr
ProjectionsDescription::getSingleExpressionForProjections(const ColumnsDescription & columns, ContextPtr query_context) const
{
    ASTPtr combined_expr_list = std::make_shared<ASTExpressionList>();
    for (const auto & projection : projections)
        for (const auto & projection_expr : projection.query_ast->children)
            combined_expr_list->children.push_back(projection_expr->clone());

    auto syntax_result = TreeRewriter(query_context).analyze(combined_expr_list, columns.getAllPhysical());
    return ExpressionAnalyzer(combined_expr_list, syntax_result, query_context).getActions(false);
}

}

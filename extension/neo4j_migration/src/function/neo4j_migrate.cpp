#include "function/neo4j_migrate.h"

#include "binder/expression/literal_expression.h"
#include "common/enums/table_type.h"
#include "common/exception/runtime.h"
#include "common/types/value/nested.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "function/table/table_function.h"
#include "httplib.h"
#include "json.hpp"
#include "main/neo4j_migration_extension.h"

namespace kuzu {
namespace neo4j_migration_extension {

using namespace kuzu::common;
using namespace kuzu::main;
using namespace kuzu::function;

struct Neo4jMigrateBindData final : TableFuncBindData {
    std::shared_ptr<httplib::Client> client;
    std::vector<std::string> nodesToImport;
    std::vector<std::string> relsToImport;

    Neo4jMigrateBindData(std::shared_ptr<httplib::Client> client,
        std::vector<std::string> nodesToImport, std::vector<std::string> relsToImport)
        : TableFuncBindData{binder::expression_vector{}, 0 /* maxOffset */},
          client{std::move(client)}, nodesToImport{std::move(nodesToImport)},
          relsToImport{std::move(relsToImport)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<Neo4jMigrateBindData>(*this);
    }
};

nlohmann::json executeNeo4jQuery(httplib::Client& cli, std::string neo4jQuery) {
    std::string requestBody = R"({"statements":[{"statement":"{}"}]})";
    requestBody = common::stringFormat(requestBody, neo4jQuery);
    httplib::Request req;
    req.method = "POST";
    req.path = "/db/neo4j/tx/commit";
    req.headers = {{"Accept", "application/json"}, {"Content-Type", "application/json"}};
    req.body.assign(reinterpret_cast<const char*>(requestBody.c_str()), requestBody.length());
    auto res = cli.send(req);
    if (!res) {
        throw common::RuntimeException{"Failed to connect to neo4j server. Please check whether it "
                                       "is valid neo4j server url."};
    }
    if (res->status != 200) {
        throw common::RuntimeException{
            common::stringFormat("Failed to connect to neo4j. Server returned: {}, Response: {}.",
                res->status, res->body)};
    }
    auto jsonifyResult = nlohmann::json::parse(res->body);
    if (!jsonifyResult["errors"].empty()) {
        throw common::RuntimeException{
            common::stringFormat("Failed to execute query: {} in neo4j. Error: {}.", neo4jQuery,
                jsonifyResult["errors"].dump())};
    }
    // Neo4j server should always return one queryResult.
    if (jsonifyResult["results"].size() != 1) {
        throw common::RuntimeException{
            "Neo4j server returns multiple results: " + jsonifyResult["results"].dump()};
    }
    return jsonifyResult["results"][0]["data"];
}

static void validateConnectionString(httplib::Client& cli) {
    executeNeo4jQuery(cli, "RETURN 1");
}

static std::unordered_set<std::string> getLabelsInNeo4j(httplib::Client& cli,
    common::TableType tableType) {
    std::unordered_set<std::string> labels;
    std::string query;
    switch (tableType) {
    case TableType::NODE: {
        query = "CALL db.labels();";
    } break;
    case TableType::REL: {
        query = "CALL db.relationshipTypes();";
    } break;
    default:
        KU_UNREACHABLE;
    }
    auto res = executeNeo4jQuery(cli, query);
    for (auto row : res) {
        for (auto element : row["row"]) {
            labels.emplace(element.get<std::string>());
        }
    }
    return labels;
}

static std::vector<std::string> getNodeOrRels(httplib::Client& cli, common::TableType tableType,
    std::shared_ptr<binder::Expression> expression) {
    auto labelsInNeo4j = getLabelsInNeo4j(cli, tableType);
    std::vector<std::string> labels;
    auto labelVals = expression->constPtrCast<binder::LiteralExpression>()->getValue();
    for (auto i = 0u; i < labelVals.getChildrenSize(); i++) {
        auto label = NestedVal::getChildVal(&labelVals, i)->toString();
        if (!labelsInNeo4j.contains(label)) {
            throw common::RuntimeException{common::stringFormat("{}: {} does not exist in neo4j.",
                TableTypeUtils::toString(tableType), label)};
        }
        // Importing multi-label nodes is not supported right now.
        if (tableType == common::TableType::NODE) {
            auto res = executeNeo4jQuery(cli,
                common::stringFormat("match (n:{}) where size(labels(n)) > 1 return labels(n)",
                    label));
            if (!res.empty()) {
                throw common::RuntimeException{common::stringFormat(
                    "Importing nodes with multi-labels is not supported right now.")};
            }
        }
        labels.push_back(std::move(label));
    }
    return labels;
}

static std::unique_ptr<TableFuncBindData> bindFunc(ClientContext* /*context*/,
    const TableFuncBindInput* input) {
    auto url = input->getLiteralVal<std::string>(0);
    auto userName = input->getLiteralVal<std::string>(1);
    auto password = input->getLiteralVal<std::string>(2);
    auto cli = std::make_shared<httplib::Client>(url, 7474);
    cli->set_basic_auth(userName, password);
    cli->set_connection_timeout(std::chrono::seconds(1000));
    cli->set_read_timeout(std::chrono::seconds(1000));
    validateConnectionString(*cli);
    auto nodes = getNodeOrRels(*cli, TableType::NODE, input->getParam(3));
    auto rels = getNodeOrRels(*cli, TableType::REL, input->getParam(4));
    return std::make_unique<Neo4jMigrateBindData>(std::move(cli), std::move(nodes),
        std::move(rels));
}

void exportNeo4jNodeToCSV(std::string nodeName, httplib::Client& cli) {
    auto query = common::stringFormat("MATCH (p:{}) "
                                      "with collect(p) as n "
                                      "CALL apoc.export.csv.data(n, [], \\\"/tmp/{}.csv\\\", null) "
                                      "YIELD file, source, format, nodes, relationships, "
                                      "properties, time, rows, batchSize, batches, done, data "
                                      "RETURN file, source, format, nodes, relationships, "
                                      "properties, time, rows, batchSize, batches, done, data",
        nodeName, nodeName);
    executeNeo4jQuery(cli, query);
}

void exportNeo4jRelToCSV(std::string relName, std::pair<std::string, std::string> nodePairs,
    httplib::Client& cli) {
    auto query =
        common::stringFormat("MATCH (:{})-[e:{}]->(:{}) "
                             "with collect(e) as rels "
                             "CALL apoc.export.csv.data([], rels, \\\"/tmp/{}_{}_{}.csv\\\", null) "
                             "YIELD file, source, format, nodes, relationships, "
                             "properties, time, rows, batchSize, batches, done, data "
                             "RETURN file, source, format, nodes, relationships, "
                             "properties, time, rows, batchSize, batches, done, data",
            nodePairs.first, relName, nodePairs.second, nodePairs.first, relName, nodePairs.second);
    executeNeo4jQuery(cli, query);
}

LogicalType convertFromNeo4jTypeStr(const std::string& neo4jTypeStr) {
    if (neo4jTypeStr == "Long") {
        return LogicalType{LogicalTypeID::INT64};
    } else if (neo4jTypeStr == "String") {
        return LogicalType{LogicalTypeID::STRING};
    } else if (neo4jTypeStr == "Date") {
        return LogicalType{LogicalTypeID::DATE};
    } else if (neo4jTypeStr == "DateTime") {
        return LogicalType{LogicalTypeID::TIMESTAMP};
    } else if (neo4jTypeStr == "Boolean") {
        return LogicalType{LogicalTypeID::BOOL};
    } else if (neo4jTypeStr == "Double") {
        return LogicalType{LogicalTypeID::DOUBLE};
    } else if (neo4jTypeStr == "LongArray") {
        return LogicalType::LIST(LogicalType::INT64());
    } else if (neo4jTypeStr == "DoubleArray") {
        return LogicalType::LIST(LogicalType::DOUBLE());
    } else if (neo4jTypeStr == "StringArray") {
        return LogicalType::LIST(LogicalType::STRING());
    } else {
        throw common::RuntimeException{
            "Neo4j type: " + neo4jTypeStr + " is not supported right now."};
    }
}

std::pair<std::string, std::string> getCreateNodeTableQuery(httplib::Client& cli,
    const std::string& nodeName) {
    auto neo4jQuery = common::stringFormat(
        "call db.schema.nodeTypeProperties() yield nodeType, propertyName,propertyTypes where "
        "nodeType = ':`{}`' return propertyName,propertyTypes",
        nodeName);
    auto data = executeNeo4jQuery(cli, neo4jQuery);
    std::string properties = "";
    std::string propertiesToCopy = "";
    for (const auto& item : data) {
        auto property = item["row"][0].get<std::string>();
        properties += property;
        auto types = item["row"][1];
        auto kuType = convertFromNeo4jTypeStr(types[0].get<std::string>());
        for (auto i = 1u; i < types.size(); i++) {
            kuType = LogicalTypeUtils::combineTypes(
                convertFromNeo4jTypeStr(types[i].get<std::string>()), kuType);
        }
        propertiesToCopy += property + ",";
        properties += (" " + kuType.toString() + ",");
    }
    return {common::stringFormat("CREATE NODE TABLE `{}` (`_id_` int64, {} PRIMARY KEY(_id_));",
                nodeName, properties),
        common::stringFormat(
            "COPY `{}` FROM (LOAD WITH HEADERS(_id STRING, _labels STRING, {} _start STRING, "
            "_end STRING, _type STRING) FROM '/tmp/{}.csv'(sample_size "
            "= 0, header=true) RETURN _id, {});",
            nodeName, properties, nodeName,
            propertiesToCopy.substr(0, propertiesToCopy.length() - 1))};
}

std::vector<std::string> getRelProperties(httplib::Client& cli, std::string srcLabel,
    std::string dstLabel, std::string relLabel) {
    auto neo4jQuery =
        common::stringFormat("MATCH (:{})-[e:{}]->(:{}) UNWIND keys(e) AS key return distinct key",
            srcLabel, relLabel, dstLabel);
    auto data = executeNeo4jQuery(cli, neo4jQuery);
    std::vector<std::string> relProperties;
    for (auto& row : data[0]["row"]) {
        relProperties.push_back(row.get<std::string>());
    }
    return relProperties;
}

std::string getCreateRelTableQuery(httplib::Client& cli, const std::string& relName,
    const std::vector<std::string>& nodeLabelsToImport) {
    auto neo4jQuery = common::stringFormat(
        "call db.schema.relTypeProperties() yield relType, propertyName,propertyTypes where "
        "relType = ':`{}`' and propertyName is not null and  propertyTypes is not null return "
        "propertyName,propertyTypes",
        relName);
    auto data = executeNeo4jQuery(cli, neo4jQuery);
    std::string properties = ",";
    std::unordered_map<std::string, std::string> propertyTypes;
    for (const auto& item : data) {
        auto property = item["row"][0].get<std::string>();
        properties += property;
        auto types = item["row"][1];
        auto kuType = convertFromNeo4jTypeStr(types[0].get<std::string>());
        for (auto i = 1u; i < types.size(); i++) {
            kuType = LogicalTypeUtils::combineTypes(
                convertFromNeo4jTypeStr(types[i].get<std::string>()), kuType);
        }
        properties += (" " + kuType.toString() + ",");
        propertyTypes.emplace(property, kuType.toString());
    }

    neo4jQuery =
        common::stringFormat("MATCH (a)-[:{}]->(b) RETURN distinct labels(a), labels(b);", relName);
    data = executeNeo4jQuery(cli, neo4jQuery);
    std::vector<std::pair<std::string, std::string>> nodePairs;
    std::string srcLabel, dstLabel;
    std::string nodePairsString;
    std::string copyQuery;
    for (const auto& item : data) {
        if (item["row"].empty()) {
            throw common::RuntimeException{"Error occurred while parsing neo4j result."};
        }
        srcLabel = item["row"][0][0].get<std::string>();
        dstLabel = item["row"][1][0].get<std::string>();
        if (std::find(nodeLabelsToImport.begin(), nodeLabelsToImport.end(), srcLabel) ==
            nodeLabelsToImport.end()) {
            throw common::RuntimeException{common::stringFormat(
                "The dependent source node label: {} of {} must be imported into kuzu.", srcLabel,
                relName)};
        }
        if (std::find(nodeLabelsToImport.begin(), nodeLabelsToImport.end(), dstLabel) ==
            nodeLabelsToImport.end()) {
            throw common::RuntimeException{common::stringFormat(
                "The dependent dst node label: {} of {} must be imported into kuzu.", dstLabel,
                relName)};
        }
        nodePairs.emplace_back(srcLabel, dstLabel);
        nodePairsString += common::stringFormat("FROM {} TO {},", srcLabel, dstLabel);
        exportNeo4jRelToCSV(relName, {srcLabel, dstLabel}, cli);
        auto relProperties = getRelProperties(cli, srcLabel, dstLabel, relName);

        std::string propertiesToCopy = "";
        std::string loadFromHeaders = "";
        for (auto i = 0u; i < relProperties.size(); i++) {
            propertiesToCopy += relProperties[i];
            loadFromHeaders +=
                common::stringFormat("{} {}", relProperties[i], propertyTypes.at(relProperties[i]));
            if (i != relProperties.size() - 1) {
                propertiesToCopy += ",";
                loadFromHeaders += ",";
            }
        }
        copyQuery +=
            common::stringFormat("COPY `{}`({}) FROM (LOAD WITH HEADERS(_id STRING, _labels "
                                 "STRING, _start INT64, _end INT64, _type STRING, {}) FROM "
                                 "'/tmp/{}_{}_{}.csv'(sample_size=0, header=true) "
                                 "RETURN `_start`, `_end`, {}) (from = \"{}\", to = \"{}\");",
                relName, propertiesToCopy, loadFromHeaders, srcLabel, relName, dstLabel,
                propertiesToCopy, srcLabel, dstLabel);
    }
    return common::stringFormat("CREATE REL TABLE `{}` ({} {});", relName,
               nodePairsString.substr(0, nodePairsString.size() - 1),
               properties.substr(0, properties.length() - 1)) +
           copyQuery;
}

std::string migrateQuery(ClientContext& /*context*/, const TableFuncBindData& bindData) {
    std::string result;
    auto neo4jMigrateBindData = bindData.constPtrCast<Neo4jMigrateBindData>();
    for (auto node : neo4jMigrateBindData->nodesToImport) {
        exportNeo4jNodeToCSV(node, *neo4jMigrateBindData->client);
        auto [ddl, copyQuery] = getCreateNodeTableQuery(*neo4jMigrateBindData->client, node);
        result += ddl;
        result += copyQuery;
    }
    for (auto rel : neo4jMigrateBindData->relsToImport) {
        result += getCreateRelTableQuery(*neo4jMigrateBindData->client, rel,
            neo4jMigrateBindData->nodesToImport);
    }
    return result;
}

static std::vector<common::LogicalType> inferInputTypes(
    const binder::expression_vector& /*params*/) {
    std::vector<common::LogicalType> inputTypes;
    // url
    inputTypes.push_back(common::LogicalType::STRING());
    // username
    inputTypes.push_back(common::LogicalType::STRING());
    // password
    inputTypes.push_back(common::LogicalType::STRING());
    // nodes
    inputTypes.push_back(common::LogicalType::LIST(common::LogicalType::STRING()));
    // rels
    inputTypes.push_back(common::LogicalType::LIST(common::LogicalType::STRING()));
    return inputTypes;
}

function_set Neo4jMigrateFunction::getFunctionSet() {
    function_set functionSet;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::STRING, LogicalTypeID::STRING,
            LogicalTypeID::LIST, LogicalTypeID::LIST});
    func->tableFunc = TableFunction::emptyTableFunc;
    func->bindFunc = bindFunc;
    func->initSharedStateFunc = SimpleTableFunc::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->rewriteFunc = migrateQuery;
    func->canParallelFunc = [] { return false; };
    func->inferInputTypes = inferInputTypes;
    functionSet.push_back(std::move(func));
    return functionSet;
}

} // namespace neo4j_migration_extension
} // namespace kuzu

#include "ui/opcua/catalog/Wire.hpp"
#include <open62541/server_config_default.h>
#include <gtest/gtest.h>
#include "ui/opcua/catalog/Records.hpp"

namespace c = ui::opcua::catalog;
namespace {
    enum class Example : std::uint8_t { First, Second };
    struct [[= c::Record{"Nested"}]] Nested {
        [[= c::Field{}]] double number{};
        [[= c::Field{}]] std::array<char, 16> text{};
    };
    struct [[= c::Record{"Envelope"}]] Envelope {
        [[= c::Field{}]] std::uint64_t id{};
        [[= c::Field{}]] Example choice{};
        [[= c::Field{}]] std::array<Nested, 2> values{};
        [[= c::Field{}, = c::Bounds{-100,100}]] double limited{};
    };
}
TEST(OpcUaWire, ReflectedRecordsHaveSeparateWireLayoutsAndRoundTrip) {
    auto* server = UA_Server_new();
    ASSERT_NE(server, nullptr);
    const auto ns = UA_Server_addNamespace(server, "urn:test");
    c::Types types(server, ns);
    const Envelope source{42, Example::Second, {{{1.5, {'a','b'}}, {-2, {'c'}}}}, 99};
    UA_Variant output{};
    ASSERT_EQ(types.variant(source, output), UA_STATUSCODE_GOOD);
    EXPECT_NE(sizeof(Envelope), sizeof(c::Wire<Envelope>::Type));
    Envelope decoded{};
    ASSERT_EQ(types.extract(output, decoded), UA_STATUSCODE_GOOD);
    EXPECT_EQ(decoded.id, 42); EXPECT_EQ(decoded.choice, Example::Second);
    EXPECT_EQ(decoded.values[1].number, -2); EXPECT_EQ(decoded.values[0].text[1], 'b');
    auto& wire = *static_cast<c::Wire<Envelope>::Type*>(output.data);
    wire.limited = 101;
    EXPECT_EQ(types.extract(output, decoded), UA_STATUSCODE_BADOUTOFRANGE);
    wire.limited = 0; wire.choice = 43;
    EXPECT_EQ(types.extract(output, decoded), UA_STATUSCODE_BADOUTOFRANGE);
    UA_Variant_clear(&output);
    UA_Server_delete(server);
}

TEST(OpcUaWire, CatalogRecordsAreBoundedAndDescribeTheirFields) {
    auto* server = UA_Server_new();
    ASSERT_NE(server, nullptr);
    c::Types types(server, UA_Server_addNamespace(server, "urn:protomate:control"));
    const auto* receipt = types.type<c::Receipt>();
    const auto* page = types.type<c::JobPage>();
    EXPECT_EQ(receipt->membersSize, 5);
    EXPECT_EQ(page->membersSize, 3);
    EXPECT_TRUE(page->members[2].isArray);
    UA_Variant variant{};
    c::JobPage source{}; source.count = 1; source.records[0].id = 123;
    ASSERT_EQ(types.variant(source, variant), UA_STATUSCODE_GOOD);
    UA_ByteString encoded{};
    ASSERT_EQ(UA_encodeBinary(variant.data, variant.type, &encoded, nullptr), UA_STATUSCODE_GOOD);
    c::Wire<c::JobPage>::Type wire{};
    UA_DecodeBinaryOptions options{}; options.customTypes = UA_Server_getDataTypes(server);
    ASSERT_EQ(UA_decodeBinary(&encoded, &wire, page, &options), UA_STATUSCODE_GOOD);
    c::JobPage decoded{};
    ASSERT_EQ(types.decode(wire, decoded), UA_STATUSCODE_GOOD);
    EXPECT_EQ(decoded.count, 1); EXPECT_EQ(decoded.records[0].id, 123);
    UA_clear(&wire, page); UA_ByteString_clear(&encoded); UA_Variant_clear(&variant);
    UA_Server_delete(server);
}

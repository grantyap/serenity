/*
 * Copyright (c) 2021, Itamar S. <itamar8910@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CppComprehensionEngine.h"
#include <AK/Assertions.h>
#include <AK/HashTable.h>
#include <AK/OwnPtr.h>
#include <LibCpp/AST.h>
#include <LibCpp/Lexer.h>
#include <LibCpp/Parser.h>
#include <LibCpp/Preprocessor.h>
#include <LibRegex/Regex.h>
#include <Userland/DevTools/HackStudio/LanguageServers/ClientConnection.h>

namespace LanguageServers::Cpp {

CppComprehensionEngine::CppComprehensionEngine(const FileDB& filedb)
    : CodeComprehensionEngine(filedb, true)
{
}

const CppComprehensionEngine::DocumentData* CppComprehensionEngine::get_or_create_document_data(const String& file)
{
    auto absolute_path = filedb().to_absolute_path(file);
    if (!m_documents.contains(absolute_path)) {
        set_document_data(absolute_path, create_document_data_for(absolute_path));
    }
    return get_document_data(absolute_path);
}

const CppComprehensionEngine::DocumentData* CppComprehensionEngine::get_document_data(const String& file) const
{
    auto absolute_path = filedb().to_absolute_path(file);
    auto document_data = m_documents.get(absolute_path);
    VERIFY(document_data.has_value());
    return document_data.value();
}

OwnPtr<CppComprehensionEngine::DocumentData> CppComprehensionEngine::create_document_data_for(const String& file)
{
    auto document = filedb().get_or_create_from_filesystem(file);
    if (!document)
        return {};
    return create_document_data(document->text(), file);
}

void CppComprehensionEngine::set_document_data(const String& file, OwnPtr<DocumentData>&& data)
{
    m_documents.set(filedb().to_absolute_path(file), move(data));
}

Vector<GUI::AutocompleteProvider::Entry> CppComprehensionEngine::get_suggestions(const String& file, const GUI::TextPosition& autocomplete_position)
{
    Cpp::Position position { autocomplete_position.line(), autocomplete_position.column() > 0 ? autocomplete_position.column() - 1 : 0 };

    dbgln_if(CPP_LANGUAGE_SERVER_DEBUG, "CppComprehensionEngine position {}:{}", position.line, position.column);

    const auto* document_ptr = get_or_create_document_data(file);
    if (!document_ptr)
        return {};

    const auto& document = *document_ptr;
    auto containing_token = document.parser().token_at(position);
    auto node = document.parser().node_at(position);
    if (!node) {
        dbgln_if(CPP_LANGUAGE_SERVER_DEBUG, "no node at position {}:{}", position.line, position.column);
        return {};
    }

    if (node->parent() && node->parent()->parent())
        dbgln_if(CPP_LANGUAGE_SERVER_DEBUG, "node: {}, parent: {}, grandparent: {}", node->class_name(), node->parent()->class_name(), node->parent()->parent()->class_name());

    if (!node->parent())
        return {};

    auto results = autocomplete_property(document, *node, containing_token);
    if (results.has_value())
        return results.value();

    results = autocomplete_name(document, *node, containing_token);
    if (results.has_value())
        return results.value();
    return {};
}

Optional<Vector<GUI::AutocompleteProvider::Entry>> CppComprehensionEngine::autocomplete_name(const DocumentData& document, const ASTNode& node, Optional<Token> containing_token) const
{
    auto partial_text = String::empty();
    if (containing_token.has_value() && containing_token.value().type() != Token::Type::ColonColon) {
        partial_text = containing_token.value().text();
    }
    return autocomplete_name(document, node, partial_text);
}

Optional<Vector<GUI::AutocompleteProvider::Entry>> CppComprehensionEngine::autocomplete_property(const DocumentData& document, const ASTNode& node, Optional<Token> containing_token) const
{
    if (!containing_token.has_value())
        return {};

    if (!node.parent()->is_member_expression())
        return {};

    const auto& parent = static_cast<const MemberExpression&>(*node.parent());

    auto partial_text = String::empty();
    if (containing_token.value().type() != Token::Type::Dot) {
        if (&node != parent.m_property)
            return {};
        partial_text = containing_token.value().text();
    }

    return autocomplete_property(document, parent, partial_text);
}

NonnullRefPtrVector<Declaration> CppComprehensionEngine::get_available_declarations(const DocumentData& document, const ASTNode& node, RecurseIntoScopes recurse_into_scopes) const
{
    const Cpp::ASTNode* current = &node;
    NonnullRefPtrVector<Declaration> available_declarations;
    while (current) {
        available_declarations.append(current->declarations());
        current = current->parent();
    }

    available_declarations.append(get_global_declarations_including_headers(document, recurse_into_scopes));
    return available_declarations;
}

Vector<GUI::AutocompleteProvider::Entry> CppComprehensionEngine::autocomplete_name(const DocumentData& document, const ASTNode& node, const String& partial_text) const
{
    auto target_scope = scope_of_name_or_identifier(node);

    auto available_declarations = get_available_declarations(document, node, RecurseIntoScopes::No);

    Vector<StringView> available_names;
    auto add_if_valid = [this, &available_names, &target_scope](auto& decl) {
        auto name = decl.m_name;
        if (name.is_null() || name.is_empty())
            return;
        auto scope = scope_of_declaration(decl);
        if (scope.is_null())
            scope = String::empty();
        if (scope != target_scope)
            return;
        if (!available_names.contains_slow(name))
            available_names.append(name);
    };

    for (auto& decl : available_declarations) {
        if (decl.filename() == node.filename() && decl.start().line > node.start().line)
            continue;
        if (decl.is_variable_or_parameter_declaration()) {
            add_if_valid(decl);
        }
        if (decl.is_struct_or_class()) {
            add_if_valid(decl);
        }
        if (decl.is_function()) {
            add_if_valid(decl);
        }
        if (decl.is_namespace()) {
            add_if_valid(decl);
        }
    }

    Vector<GUI::AutocompleteProvider::Entry> suggestions;
    for (auto& name : available_names) {
        if (name.starts_with(partial_text)) {
            suggestions.append({ name.to_string(), partial_text.length(), GUI::AutocompleteProvider::CompletionKind::Identifier });
        }
    }

    if (target_scope.is_empty()) {
        for (auto& preprocessor_name : document.parser().preprocessor_definitions().keys()) {
            if (preprocessor_name.starts_with(partial_text)) {
                suggestions.append({ preprocessor_name.to_string(), partial_text.length(), GUI::AutocompleteProvider::CompletionKind::PreprocessorDefinition });
            }
        }
    }

    return suggestions;
}

String CppComprehensionEngine::scope_of_name_or_identifier(const ASTNode& node) const
{
    const Name* name = nullptr;
    if (node.is_name()) {
        name = reinterpret_cast<const Name*>(&node);
    } else if (node.is_identifier()) {
        auto* parent = node.parent();
        if (!(parent && parent->is_name()))
            return {};
        name = reinterpret_cast<const Name*>(parent);
    } else {
        return String::empty();
    }

    VERIFY(name->is_name());

    Vector<StringView> scope_parts;
    for (auto& scope_part : name->m_scope) {
        scope_parts.append(scope_part.m_name);
    }
    return String::join("::", scope_parts);
}

Vector<GUI::AutocompleteProvider::Entry> CppComprehensionEngine::autocomplete_property(const DocumentData& document, const MemberExpression& parent, const String partial_text) const
{
    auto type = type_of(document, *parent.m_object);
    if (type.is_null()) {
        dbgln_if(CPP_LANGUAGE_SERVER_DEBUG, "Could not infer type of object");
        return {};
    }

    Vector<GUI::AutocompleteProvider::Entry> suggestions;
    for (auto& prop : properties_of_type(document, type)) {
        if (prop.name.starts_with(partial_text)) {
            suggestions.append({ prop.name, partial_text.length(), GUI::AutocompleteProvider::CompletionKind::Identifier });
        }
    }
    return suggestions;
}

bool CppComprehensionEngine::is_property(const ASTNode& node) const
{
    if (!node.parent()->is_member_expression())
        return false;

    auto& parent = (MemberExpression&)(*node.parent());
    return parent.m_property.ptr() == &node;
}

bool CppComprehensionEngine::is_empty_property(const DocumentData& document, const ASTNode& node, const Position& autocomplete_position) const
{
    if (node.parent() == nullptr)
        return false;
    if (!node.parent()->is_member_expression())
        return false;
    auto previous_token = document.parser().token_at(autocomplete_position);
    if (!previous_token.has_value())
        return false;
    return previous_token.value().type() == Token::Type::Dot;
}

String CppComprehensionEngine::type_of_property(const DocumentData& document, const Identifier& identifier) const
{
    auto& parent = (const MemberExpression&)(*identifier.parent());
    auto properties = properties_of_type(document, type_of(document, *parent.m_object));
    for (auto& prop : properties) {
        if (prop.name == identifier.m_name)
            return prop.type->m_name->full_name();
    }
    return {};
}

String CppComprehensionEngine::type_of_variable(const Identifier& identifier) const
{
    const ASTNode* current = &identifier;
    while (current) {
        for (auto& decl : current->declarations()) {
            if (decl.is_variable_or_parameter_declaration()) {
                auto& var_or_param = (VariableOrParameterDeclaration&)decl;
                if (var_or_param.m_name == identifier.m_name) {
                    return var_or_param.m_type->m_name->full_name();
                }
            }
        }
        current = current->parent();
    }
    return {};
}

String CppComprehensionEngine::type_of(const DocumentData& document, const Expression& expression) const
{
    if (expression.is_member_expression()) {
        auto& member_expression = (const MemberExpression&)expression;
        if (member_expression.m_property->is_identifier())
            return type_of_property(document, static_cast<const Identifier&>(*member_expression.m_property));
        return {};
    }

    const Identifier* identifier { nullptr };
    if (expression.is_name()) {
        identifier = static_cast<const Name&>(expression).m_name.ptr();
    } else if (expression.is_identifier()) {
        identifier = &static_cast<const Identifier&>(expression);
    } else {
        dbgln("expected identifier or name, got: {}", expression.class_name());
        VERIFY_NOT_REACHED(); // TODO
    }
    VERIFY(identifier);
    if (is_property(*identifier))
        return type_of_property(document, *identifier);

    return type_of_variable(*identifier);
}

Vector<CppComprehensionEngine::PropertyInfo> CppComprehensionEngine::properties_of_type(const DocumentData& document, const String& type) const
{
    auto declarations = get_global_declarations_including_headers(document, RecurseIntoScopes::Yes);
    Vector<PropertyInfo> properties;
    for (auto& decl : declarations) {
        if (!decl.is_struct_or_class())
            continue;
        auto& struct_or_class = (StructOrClassDeclaration&)decl;
        if (struct_or_class.m_name != type)
            continue;
        for (auto& member : struct_or_class.m_members) {
            properties.append({ member.m_name, member.m_type });
        }
    }
    return properties;
}

NonnullRefPtrVector<Declaration> CppComprehensionEngine::get_global_declarations_including_headers(const DocumentData& document, RecurseIntoScopes recurse_into_scopes) const
{
    NonnullRefPtrVector<Declaration> declarations;
    for (auto& decl : document.m_declarations_from_headers)
        declarations.append(*decl);

    declarations.append(get_global_declarations(document, recurse_into_scopes));

    return declarations;
}

NonnullRefPtrVector<Declaration> CppComprehensionEngine::get_global_declarations(const DocumentData& document, RecurseIntoScopes recurse_into_scopes) const
{
    if (recurse_into_scopes == RecurseIntoScopes::Yes)
        return get_declarations_recursive(*document.parser().root_node());
    return document.parser().root_node()->declarations();
}

NonnullRefPtrVector<Declaration> CppComprehensionEngine::get_declarations_recursive(const ASTNode& node) const
{
    NonnullRefPtrVector<Declaration> declarations;

    for (auto& decl : node.declarations()) {
        declarations.append(decl);
        if (decl.is_namespace()) {
            declarations.append(get_declarations_recursive(decl));
        }
        if (decl.is_struct_or_class()) {
            for (auto& member_decl : static_cast<StructOrClassDeclaration&>(decl).declarations()) {
                declarations.append(member_decl);
            }
        }
    }

    return declarations;
}

String CppComprehensionEngine::document_path_from_include_path(const StringView& include_path) const
{
    static Regex<PosixExtended> library_include("<(.+)>");
    static Regex<PosixExtended> user_defined_include("\"(.+)\"");

    auto document_path_for_library_include = [&](const StringView& include_path) -> String {
        RegexResult result;
        if (!library_include.search(include_path, result))
            return {};

        auto path = result.capture_group_matches.at(0).at(0).view.u8view();
        return String::formatted("/usr/include/{}", path);
    };

    auto document_path_for_user_defined_include = [&](const StringView& include_path) -> String {
        RegexResult result;
        if (!user_defined_include.search(include_path, result))
            return {};

        return result.capture_group_matches.at(0).at(0).view.u8view();
    };

    auto result = document_path_for_library_include(include_path);
    if (result.is_null())
        result = document_path_for_user_defined_include(include_path);

    return result;
}

void CppComprehensionEngine::on_edit(const String& file)
{
    set_document_data(file, create_document_data_for(file));
}

void CppComprehensionEngine::file_opened([[maybe_unused]] const String& file)
{
    get_or_create_document_data(file);
}

Optional<GUI::AutocompleteProvider::ProjectLocation> CppComprehensionEngine::find_declaration_of(const String& filename, const GUI::TextPosition& identifier_position)
{
    const auto* document_ptr = get_or_create_document_data(filename);
    if (!document_ptr)
        return {};

    const auto& document = *document_ptr;
    auto node = document.parser().node_at(Cpp::Position { identifier_position.line(), identifier_position.column() });
    if (!node) {
        dbgln_if(CPP_LANGUAGE_SERVER_DEBUG, "no node at position {}:{}", identifier_position.line(), identifier_position.column());
        return {};
    }
    auto decl = find_declaration_of(document, *node);
    if (decl)
        return GUI::AutocompleteProvider::ProjectLocation { decl->filename(), decl->start().line, decl->start().column };

    return find_preprocessor_definition(document, identifier_position);
}

Optional<GUI::AutocompleteProvider::ProjectLocation> CppComprehensionEngine::find_preprocessor_definition(const DocumentData& document, const GUI::TextPosition& text_position)
{
    Position cpp_position { text_position.line(), text_position.column() };

    // Search for a replaced preprocessor token that intersects with text_position
    for (auto& replaced_token : document.parser().replaced_preprocessor_tokens()) {
        if (replaced_token.token.start() > cpp_position)
            continue;
        if (replaced_token.token.end() < cpp_position)
            continue;

        return GUI::AutocompleteProvider::ProjectLocation { replaced_token.preprocessor_value.filename, replaced_token.preprocessor_value.line, replaced_token.preprocessor_value.column };
    }
    return {};
}

struct TargetDeclaration {
    enum Type {
        Variable,
        Type,
        Function,
        Property
    } type;
    String name;
};

static Optional<TargetDeclaration> get_target_declaration(const ASTNode& node)
{
    if (!node.is_identifier()) {
        dbgln_if(CPP_LANGUAGE_SERVER_DEBUG, "node is not an identifier");
        return {};
    }

    String name = static_cast<const Identifier&>(node).m_name;

    if ((node.parent() && node.parent()->is_function_call()) || (node.parent()->is_name() && node.parent()->parent() && node.parent()->parent()->is_function_call())) {
        return TargetDeclaration { TargetDeclaration::Type::Function, name };
    }

    if ((node.parent() && node.parent()->is_type()) || (node.parent()->is_name() && node.parent()->parent() && node.parent()->parent()->is_type()))
        return TargetDeclaration { TargetDeclaration::Type::Type, name };

    if ((node.parent() && node.parent()->is_member_expression()))
        return TargetDeclaration { TargetDeclaration::Type::Property, name };

    return TargetDeclaration { TargetDeclaration::Type::Variable, name };
}

RefPtr<Declaration> CppComprehensionEngine::find_declaration_of(const DocumentData& document_data, const ASTNode& node) const
{
    dbgln_if(CPP_LANGUAGE_SERVER_DEBUG, "find_declaration_of: {} ({})", document_data.parser().text_of_node(node), node.class_name());
    auto target_decl = get_target_declaration(node);
    if (!target_decl.has_value())
        return {};

    auto declarations = get_available_declarations(document_data, node, RecurseIntoScopes::Yes);
    for (auto& decl : declarations) {
        if (decl.is_function() && target_decl.value().type == TargetDeclaration::Function) {
            if (((Cpp::FunctionDeclaration&)decl).m_name == target_decl.value().name)
                return decl;
        }
        if (decl.is_variable_or_parameter_declaration() && target_decl.value().type == TargetDeclaration::Variable) {
            if (((Cpp::VariableOrParameterDeclaration&)decl).m_name == target_decl.value().name)
                return decl;
        }

        if (decl.is_struct_or_class() && target_decl.value().type == TargetDeclaration::Property) {
            // TODO: Also check that the type of the struct/class matches (not just the property name)
            for (auto& member : ((Cpp::StructOrClassDeclaration&)decl).m_members) {
                VERIFY(node.is_identifier());
                if (member.m_name == target_decl.value().name) {
                    return member;
                }
            }
        }

        if (decl.is_struct_or_class() && target_decl.value().type == TargetDeclaration::Type) {
            if (((Cpp::StructOrClassDeclaration&)decl).m_name == target_decl.value().name)
                return decl;
        }
    }
    return {};
}

void CppComprehensionEngine::update_declared_symbols(DocumentData& document)
{
    for (auto& include : document.preprocessor().included_paths()) {
        auto included_document = get_or_create_document_data(document_path_from_include_path(include));
        if (!included_document)
            continue;
        for (auto&& decl : get_global_declarations_including_headers(*included_document, RecurseIntoScopes::Yes))
            document.m_declarations_from_headers.set(move(decl));
    }

    Vector<GUI::AutocompleteProvider::Declaration> declarations;

    for (auto& decl : get_declarations_recursive(*document.parser().root_node())) {
        declarations.append({ decl.name(), { document.filename(), decl.start().line, decl.start().column }, type_of_declaration(decl), scope_of_declaration(decl) });
    }

    for (auto& definition : document.preprocessor().definitions()) {
        declarations.append({ definition.key, { document.filename(), definition.value.line, definition.value.column }, GUI::AutocompleteProvider::DeclarationType::PreprocessorDefinition, {} });
    }

    set_declarations_of_document(document.filename(), move(declarations));
}

GUI::AutocompleteProvider::DeclarationType CppComprehensionEngine::type_of_declaration(const Declaration& decl)
{
    if (decl.is_struct())
        return GUI::AutocompleteProvider::DeclarationType::Struct;
    if (decl.is_class())
        return GUI::AutocompleteProvider::DeclarationType::Class;
    if (decl.is_function())
        return GUI::AutocompleteProvider::DeclarationType::Function;
    if (decl.is_variable_declaration())
        return GUI::AutocompleteProvider::DeclarationType::Variable;
    if (decl.is_namespace())
        return GUI::AutocompleteProvider::DeclarationType::Namespace;
    if (decl.is_member())
        return GUI::AutocompleteProvider::DeclarationType::Member;
    return GUI::AutocompleteProvider::DeclarationType::Variable;
}

OwnPtr<CppComprehensionEngine::DocumentData> CppComprehensionEngine::create_document_data(String&& text, const String& filename)
{
    auto document_data = make<DocumentData>();
    document_data->m_filename = move(filename);
    document_data->m_text = move(text);
    document_data->m_preprocessor = make<Preprocessor>(document_data->m_filename, document_data->text());
    document_data->preprocessor().set_ignore_unsupported_keywords(true);
    document_data->preprocessor().process();

    Preprocessor::Definitions all_definitions;
    for (auto item : document_data->preprocessor().definitions())
        all_definitions.set(move(item.key), move(item.value));

    for (auto include : document_data->preprocessor().included_paths()) {
        auto included_document = get_or_create_document_data(document_path_from_include_path(include));
        if (!included_document)
            continue;
        for (auto item : included_document->parser().preprocessor_definitions())
            all_definitions.set(move(item.key), move(item.value));
    }

    document_data->m_parser = make<Parser>(document_data->preprocessor().processed_text(), filename, move(all_definitions));

    auto root = document_data->parser().parse();

    if constexpr (CPP_LANGUAGE_SERVER_DEBUG)
        root->dump(0);

    update_declared_symbols(*document_data);

    return document_data;
}

String CppComprehensionEngine::scope_of_declaration(const Declaration& decl) const
{
    auto parent = decl.parent();
    if (!parent)
        return {};

    if (!parent->is_declaration())
        return {};

    auto& parent_decl = static_cast<Declaration&>(*parent);

    auto parent_scope = scope_of_declaration(parent_decl);
    String containing_scope;
    if (parent_decl.is_namespace())
        containing_scope = static_cast<NamespaceDeclaration&>(parent_decl).m_name;
    if (parent_decl.is_struct_or_class())
        containing_scope = static_cast<StructOrClassDeclaration&>(parent_decl).name();

    if (parent_scope.is_null())
        return containing_scope;

    return String::formatted("{}::{}", parent_scope, containing_scope);
}

}

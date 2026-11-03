// Adapted from LabServer .devcontainer/clang-tidy/BraceInitializationCheck.cpp.
#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/StmtCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include <set>

namespace protomate::lint
{
    class BraceInitializationCheck final : public clang::tidy::ClangTidyCheck
    {
      public:
        using ClangTidyCheck::ClangTidyCheck;

        auto isLanguageVersionSupported(const clang::LangOptions& language) const -> bool override
        {
            return language.CPlusPlus;
        }

        auto registerMatchers(clang::ast_matchers::MatchFinder* finder) -> void override
        {
            using namespace clang::ast_matchers;
            finder->addMatcher(varDecl(unless(parmVarDecl()), unless(isImplicit())).bind("variable"), this);
            finder->addMatcher(fieldDecl(hasInClassInitializer(expr())).bind("field"), this);
            finder->addMatcher(cxxConstructorDecl(unless(isImplicit())).bind("constructor"), this);
        }

        auto check(const clang::ast_matchers::MatchFinder::MatchResult& match) -> void override
        {
            if (const auto* variable{ match.Nodes.getNodeAs<clang::VarDecl>("variable") }) {
                if (variable->hasInit() && hasWrittenDeclarator(*variable, *match.SourceManager) &&
                    !isExpansionVariable(*variable, *match.Context) && !variable->isCXXForRangeDecl() &&
                    !variable->isInitCapture() && variable->getInitStyle() != clang::VarDecl::ListInit) {
                    report(variable->getLocation(), variable->getSourceRange(), *match.SourceManager);
                }
            }
            if (const auto* field{ match.Nodes.getNodeAs<clang::FieldDecl>("field") }) {
                if (field->getInClassInitStyle() == clang::ICIS_CopyInit) {
                    report(field->getLocation(), field->getSourceRange(), *match.SourceManager);
                }
            }
            if (const auto* constructor{ match.Nodes.getNodeAs<clang::CXXConstructorDecl>("constructor") }) {
                for (const auto* initializer : constructor->inits()) {
                    if (!initializer->isWritten()) {
                        continue;
                    }
                    clang::Token token{};
                    if (!clang::Lexer::getRawToken(
                          match.SourceManager->getSpellingLoc(initializer->getLParenLoc()),
                          token,
                          *match.SourceManager,
                          getLangOpts()) &&
                        token.is(clang::tok::l_paren)) {
                        report(initializer->getLParenLoc(),
                               { initializer->getLParenLoc(), initializer->getRParenLoc() },
                               *match.SourceManager);
                    }
                }
            }
        }

      private:
        // The reflection fork exposes synthetic range temporaries as VarDecls
        // without isImplicit(). Their locations name expressions, not declarators.
        auto hasWrittenDeclarator(const clang::VarDecl& variable, const clang::SourceManager& sources) const
          -> bool
        {
            clang::Token token{};
            if (clang::Lexer::getRawToken(
                  sources.getSpellingLoc(variable.getLocation()), token, sources, getLangOpts())) {
                return false;
            }
            if (llvm::isa<clang::DecompositionDecl>(variable)) {
                return token.is(clang::tok::l_square);
            }
            return token.is(clang::tok::raw_identifier) &&
                   clang::Lexer::getSpelling(token, sources, getLangOpts()) == variable.getName();
        }

        static auto isExpansionVariable(const clang::VarDecl& variable, clang::ASTContext& context) -> bool
        {
            for (const auto& parent : context.getParents(variable)) {
                if (const auto* statement{ parent.get<clang::DeclStmt>() }) {
                    for (const auto& owner : context.getParents(*statement)) {
                        if (const auto* expansion{ owner.get<clang::CXXExpansionStmt>() };
                            expansion && expansion->getExpansionVariable() == &variable) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        auto report(clang::SourceLocation location,
                    clang::SourceRange range,
                    const clang::SourceManager& sources) -> void
        {
            location = sources.getSpellingLoc(location);
            if (location.isInvalid() || sources.getFilename(location).empty() ||
                (range.getBegin().isValid() &&
                 sources.isInSystemHeader(sources.getSpellingLoc(range.getBegin()))) ||
                sources.isInSystemHeader(location) || !m_seen.insert(location.getRawEncoding()).second) {
                return;
            }
            // No automatic fix: changing a count/value constructor to braces can
            // select initializer_list and silently change behavior.
            diag(location, "use direct brace initialization") << range;
        }

        std::set<unsigned> m_seen{};
    };

    class StyleModule final : public clang::tidy::ClangTidyModule
    {
      public:
        auto addCheckFactories(clang::tidy::ClangTidyCheckFactories& factories) -> void override
        {
            factories.registerCheck<BraceInitializationCheck>("protomate-brace-initialization");
        }
    };

    static const clang::tidy::ClangTidyModuleRegistry::Add<StyleModule> REGISTRATION{
        "protomate-style",
        "ProtoMate initialization policy"
    };
}

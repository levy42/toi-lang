package dev.pua.idea

import com.intellij.codeInsight.completion.CompletionContributor
import com.intellij.codeInsight.completion.CompletionParameters
import com.intellij.codeInsight.completion.CompletionProvider
import com.intellij.codeInsight.completion.CompletionResultSet
import com.intellij.codeInsight.completion.CompletionType
import com.intellij.codeInsight.lookup.LookupElementBuilder
import com.intellij.patterns.PlatformPatterns.psiElement
import com.intellij.util.ProcessingContext

class PuaCompletionContributor : CompletionContributor() {
    init {
        extend(
            CompletionType.BASIC,
            psiElement().withLanguage(PuaLanguage),
            KeywordCompletionProvider,
        )
    }

    private object KeywordCompletionProvider : CompletionProvider<CompletionParameters>() {
        private val keywords = listOf(
            "fn", "local", "global", "import", "return",
            "if", "elif", "else", "while", "for", "in",
            "break", "continue", "try", "except", "finally", "throw", "with", "as",
            "and", "or", "not", "has",
            "true", "false", "nil", "print", "gc", "del",
        )

        override fun addCompletions(
            parameters: CompletionParameters,
            context: ProcessingContext,
            result: CompletionResultSet,
        ) {
            for (keyword in keywords) {
                result.addElement(LookupElementBuilder.create(keyword))
            }

            result.addElement(LookupElementBuilder.create("fn ").withPresentableText("fn name(args)").withTypeText("function"))
            result.addElement(LookupElementBuilder.create("if ").withPresentableText("if condition").withTypeText("block"))
            result.addElement(LookupElementBuilder.create("for ").withPresentableText("for x in iterable").withTypeText("loop"))
            result.addElement(LookupElementBuilder.create("try").withPresentableText("try / except").withTypeText("exceptions"))
        }
    }
}

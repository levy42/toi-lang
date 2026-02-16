package dev.pua.idea

import com.intellij.application.options.IndentOptionsEditor
import com.intellij.application.options.SmartIndentOptionsEditor
import com.intellij.lang.Language
import com.intellij.psi.codeStyle.CodeStyleSettingsCustomizable
import com.intellij.psi.codeStyle.CommonCodeStyleSettings
import com.intellij.psi.codeStyle.LanguageCodeStyleSettingsProvider

class PuaCodeStyleSettingsProvider : LanguageCodeStyleSettingsProvider() {
    override fun getLanguage(): Language = PuaLanguage

    override fun getCodeSample(settingsType: SettingsType): String = SAMPLE

    override fun customizeSettings(consumer: CodeStyleSettingsCustomizable, settingsType: SettingsType) {
        if (settingsType == SettingsType.SPACING_SETTINGS) {
            consumer.showStandardOptions(
                "SPACE_AROUND_ASSIGNMENT_OPERATORS",
                "SPACE_AROUND_LOGICAL_OPERATORS",
                "SPACE_AROUND_EQUALITY_OPERATORS",
                "SPACE_AROUND_RELATIONAL_OPERATORS",
                "SPACE_AROUND_ADDITIVE_OPERATORS",
                "SPACE_AROUND_MULTIPLICATIVE_OPERATORS",
                "SPACE_BEFORE_QUEST",
                "SPACE_AFTER_QUEST",
                "SPACE_BEFORE_COLON",
                "SPACE_AFTER_COLON",
                "SPACE_AFTER_COMMA",
                "SPACE_BEFORE_COMMA",
            )
        }
    }

    override fun getIndentOptionsEditor(): IndentOptionsEditor = SmartIndentOptionsEditor()

    override fun getDefaultCommonSettings(): CommonCodeStyleSettings {
        val settings = CommonCodeStyleSettings(PuaLanguage)
        settings.initIndentOptions()
        settings.indentOptions?.INDENT_SIZE = 2
        settings.indentOptions?.TAB_SIZE = 4
        settings.indentOptions?.CONTINUATION_INDENT_SIZE = 2
        return settings
    }

    companion object {
        private const val SAMPLE = """
fn add(a, b = 10)
  return a + b

if add(1, 2) > 2
  print \"ok\"
else
  print \"no\"
"""
    }
}

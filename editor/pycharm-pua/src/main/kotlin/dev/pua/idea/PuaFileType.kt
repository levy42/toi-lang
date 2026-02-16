package dev.pua.idea

import com.intellij.openapi.fileTypes.LanguageFileType
import javax.swing.Icon

class PuaFileType private constructor() : LanguageFileType(PuaLanguage) {
    override fun getName(): String = "Pua"

    override fun getDescription(): String = "Pua language file"

    override fun getDefaultExtension(): String = "pua"

    override fun getIcon(): Icon? = null

    companion object {
        @JvmField
        val INSTANCE = PuaFileType()
    }
}

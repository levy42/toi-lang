package dev.pua.idea

import com.intellij.extapi.psi.PsiFileBase
import com.intellij.psi.FileViewProvider

class PuaFile(viewProvider: FileViewProvider) : PsiFileBase(viewProvider, PuaLanguage) {
    override fun getFileType() = PuaFileType.INSTANCE

    override fun toString(): String = "Pua File"
}

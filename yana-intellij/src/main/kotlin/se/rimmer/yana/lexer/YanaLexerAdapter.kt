package se.rimmer.yana.lexer

import com.intellij.lexer.FlexAdapter

/// The generated JFlex scanner, wrapped as a platform lexer. `YanaFlexLexer` is generated from
/// `YanaLexer.flex` by the `generateYanaLexer` Gradle task and is not in version control.
class YanaLexerAdapter : FlexAdapter(YanaFlexLexer(null))

" Vim syntax file for Pua
" Language:     Pua
" Maintainer:   Auto-generated

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syn case match

" syncing method
syn sync minlines=1000

"Types
syntax match puaMeta /\<\w\+\>/ containedin=myTypeBlock
syntax match myTypeBlock /\w\+\s*{/ 
highlight link myType Type

" Keywords
syn keyword puaStatement return local break gc import fn
syn match puaFunction /fn\s*\zs\w\+\ze(/
syn keyword puaBuildIn setmetatable getmetatable str
syn keyword Debug print

" Conditional
syn keyword puaCond if elif else

" Repeat
syn keyword puaRepeat while for in continue

" Constants
syn keyword puaConstant nil true false

" Single quote: '...'
syn region puaString start="'" end="'" skip="\\'"
" Double quote: "..."
syn region puaString start='"' end='"' skip='\\"'
" Long string: [[...]]
syn region puaLongString start='\[\[' end='\]\]'

" Numbers
syn match puaNumber "<\d\+" 
syn match puaNumber "<\d\+\.\d\+"

" Operators
syn keyword puaOperator and or not
syn match   puaSymbolOperator "[-+*/%^#=<>!]"
syn match   puaSymbolOperator "\.\.\."
" syn match   puaSymbolOperator "\.["

" Delimiters
syn match puaDelimiter "[(){}\[\]. ,;:]"

" Comments (Everything after --)
syn keyword puaTodo    contained TODO FIXME XXX NOTE OPTIMIZE
syn match   puaComment "--.*$" contains=puaTodo

" Highlighting Links
hi def link puaStatement        Statement
hi def link puaRepeat           Repeat
hi def link puaString           String
hi def link puaLongString       String
hi def link puaNumber           Number
hi def link puaOperator         Operator
hi def link puaSymbolOperator   Operator
hi def link puaConstant         Constant
hi def link puaCond             Conditional
hi def link puaFunction         Function
hi def link puaComment          Comment
hi def link puaTodo             Todo
hi def link puaDelimiter        Delimiter
hi def link puaBuildIn          Label

let b:current_syntax = "pua"

let &cpo = s:cpo_save
unlet s:cpo_save


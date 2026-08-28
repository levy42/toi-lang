.text
.globl _main
.align 2

_main:
	mov	x0, #1
	adrp	x1, helloworld@PAGE
	add	x1, x1, helloworld@PAGEOFF
	mov	x2, #13
	mov	x16, #4
	svc	#0x80

	mov	x0, #0
	ret

.section __TEXT,__cstring
helloworld:
	.asciz "Hello World!\n"

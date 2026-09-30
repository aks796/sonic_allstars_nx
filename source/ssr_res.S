/* ssr_res.S -- the port's own pictures, in the program: the white button
 * icons of the menus' prompts (resources/buttons/icons.png, drawn by
 * tools/make_button_icons.py; ssr_prompt.c). */
	.section .rodata.ssr_res, "a"
	.balign 4
	.global ssr_res_icons
	.type ssr_res_icons, %object
ssr_res_icons:
	.incbin "resources/buttons/icons.png"
ssr_res_icons_end:
	.balign 4
	.global ssr_res_icons_size
ssr_res_icons_size:
	.word ssr_res_icons_end - ssr_res_icons

#if DCR_XCARD_BAKED
/* the Xbox 360 edition's SPLIT SCREEN card (ssr_xcard.c) */
	.section .rodata.ssr_res, "a"
	.balign 4
	.global ssr_res_xcard
	.type ssr_res_xcard, %object
ssr_res_xcard:
	.incbin "resources/xbox360/splitscreen_card.png"
ssr_res_xcard_end:
	.balign 4
	.global ssr_res_xcard_size
ssr_res_xcard_size:
	.word ssr_res_xcard_end - ssr_res_xcard
#endif

	.section .note.GNU-stack,"",%progbits

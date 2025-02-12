/*
 * Copyright (C) 2000, 2001, 2002 Loic Dachary <loic@senga.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Provides functions to strip accents from a string in all the
 * charset supported by iconv(3).
 *
 * See the unac(3) manual page for more information.
 *
 */

#ifndef _unac_h
#define _unac_h

#ifdef __cplusplus
extern "C" {
#endif

/* Generated by builder. Do not modify. Start defines */
#define UNAC_BLOCK_SHIFT 4
#define UNAC_BLOCK_MASK ((1 << UNAC_BLOCK_SHIFT) - 1)
#define UNAC_BLOCK_SIZE (1 << UNAC_BLOCK_SHIFT)
#define UNAC_BLOCK_COUNT 440
#define UNAC_INDEXES_SIZE (0x10000 >> UNAC_BLOCK_SHIFT)
/* Generated by builder. Do not modify. End defines */

/*
 * Return the unaccented equivalent of the UTF-16 character <c>
 * in the pointer <p>. The length of the unsigned short array pointed
 * by <p> is returned in the <l> argument.
 * The C++ prototype of this macro would be:
 *
 * void unac_char(const unsigned short c, unsigned short*& p, int& l, int o)
 *
 * See unac(3) in IMPLEMENTATION NOTES for more information about the
 * tables (unac_data_table, unac_positions) layout.
 *
 * Each transformed char has 3 possible outputs: unaccented, unaccented and
 * folded, or just folded. These are kept at offset 0,1,2 in the position table
 */ 
#define unac_uf_char_utf16_(c,p,l,o)                    \
    {                                    \
    unsigned short index = unac_indexes[(c) >> UNAC_BLOCK_SHIFT];   \
    unsigned char position = 3*((c) & UNAC_BLOCK_MASK) + (o);    \
    (p) = &(unac_data_table[index][unac_positions[index][position]]); \
    (l) = unac_positions[index][position + 1]            \
        - unac_positions[index][position];                \
    if((l) == 1 && *(p) == 0xFFFF) {                \
        (p) = nullptr;                            \
        (l) = 0;                            \
    }                                \
    }

#define unac_char_utf16(c,p,l) unac_uf_char_utf16_((c),(p),(l),0)
#define unacfold_char_utf16(c,p,l) unac_uf_char_utf16_((c),(p),(l),1)
#define fold_char_utf16(c,p,l) unac_uf_char_utf16_((c),(p),(l),2)

/*
 * Return the unaccented equivalent of the UTF-16 string <in> of
 * length <in_length> in the pointer <out>. The length of the UTF-16
 * string returned in <out> is stored in <out_length>. If the pointer
 * *out is null, a new string is allocated using malloc(3). If the
 * pointer *out is not null, the available length must also be given
 * in the *out_length argument.  The pointer passed to *out must have
 * been allocated by malloc(3) and may be reallocated by realloc(3) if
 * needs be. It is the responsibility of the caller to free the
 * pointer returned in *out.  The return value is 0 on success and -1
 * on error, in which case the errno variable is set to the
 * corresponding error code.
 */
int unac_string_utf16(const char* in, size_t in_length, char** out, size_t* out_length);
int unacfold_string_utf16(const char* in, size_t in_length, char** out, size_t* out_length);
int fold_string_utf16(const char* in, size_t in_length, char** out, size_t* out_length);
int unacmaybefold_string_utf16(
    const char* in, size_t in_length, char** outp, size_t* out_lengthp, int what);
/*
 * The semantic of these function is stricly equal to the _utf16-suffixed ones
 * The <charset> argument applies to the content of the
 * input string. It is converted to UTF-16 using iconv(3) before calling
 * the unac_string function and the result is converted from UTF-16 to
 * the specified <charset> before returning it in the <out> pointer.
 * For efficiency purpose it is recommended that the caller uses
 * unac_string and iconv(3) to save buffer allocations overhead. 
 * The return value is 0 on success and -1 on error, in which case
 * the errno variable is set to the corresponding error code.
 */
int unac_string(
    const char* charset, const char* in, size_t in_length, char** out, size_t* out_length);
int unacfold_string(
    const char* charset, const char* in, size_t in_length, char** out, size_t* out_length);
int fold_string(
    const char* charset, const char* in, size_t in_length, char** out, size_t* out_length);
int unacmaybefold_string(
    const char* charset, const char* in, size_t in_length,char** outp,size_t* out_lengthp,int what);

int unac_u8string(const char* in, size_t in_length, char** out, size_t* out_length);
int unacfold_u8string(const char* in, size_t in_length, char** out, size_t* out_length);
int fold_u8string(const char* in, size_t in_length, char** out, size_t* out_length);
int unacmaybefold_u8string(const char* in,size_t in_length,char** outp,size_t* out_lengthp,int what);


#ifdef BUILDING_RECOLL
#include <string>
/** 
 * Set exceptions for unaccenting, for characters which should not be
 * handled according to what the Unicode tables say. For example "a
 * with circle above" should not be stripped to a in swedish, etc.
 * 
 * @param spectrans defines the translations as a blank separated list of 
 *  UTF-8 strings. Inside each string, the first character is the exception
 *  the rest is the translation (which may be empty). You can use double 
 *  quotes for translations which should include white space. The double-quote
 *  can't be an exception character, deal with it...
 */
void unac_set_except_translations(const char *spectrans);
#endif /* BUILDING_RECOLL */

/*
 * Return unac version number.
 */
const char* unac_version(void);

#define UNAC_DEBUG_NONE    0x00
#define UNAC_DEBUG_LOW    0x01
#define UNAC_DEBUG_HIGH 0x02

#ifdef HAVE_VSNPRINTF
#define UNAC_DEBUG_AVAILABLE 1
/*
 * Set the unac debug level. <l> is one of:
 * UNAC_DEBUG_NONE for no debug messages at all
 * UNAC_DEBUG_LOW for minimal information
 * UNAC_DEBUG_HIGH for extremely verbose information,
 *                 only usable when translating a few short strings.
 *
 * unac_debug with anything but UNAC_DEBUG_NONE is not
 * thread safe.
 */
#define unac_debug(l) unac_debug_callback((l), 0, (void*)0);

/*
 * Set the debug level and define a printing function callback.
 * The <level> debug level is the same as in unac_debug. The
 * <function> is in charge of dealing with the debug messages,
 * presumably to print them to the user. The <data> is an opaque
 * pointer that is passed along to <function>, should it
 * need to manage a persistent context.
 *
 * The prototype of <function> allows two arguments. The first
 * is the debug message (const char*), the second is the opaque
 * pointer given as <data> argument to unac_debug_callback.
 *
 * If <function> is NULL, messages are printed on the standard
 * error output using fprintf(stderr...).
 * 
 * unac_debug_callback with anything but UNAC_DEBUG_NONE is not
 * thread safe.
 *
 */
typedef void (*unac_debug_print_t)(const char* message, void* data);
void unac_debug_callback(int level, unac_debug_print_t function, void* data);
#endif /* HAVE_VSNPRINTF */

/* Generated by builder. Do not modify. Start declarations */
extern unsigned short unac_indexes[UNAC_INDEXES_SIZE];
extern unsigned char unac_positions[UNAC_BLOCK_COUNT][3*UNAC_BLOCK_SIZE + 1];
extern unsigned short* unac_data_table[UNAC_BLOCK_COUNT];
extern unsigned short unac_data0[];
extern unsigned short unac_data1[];
extern unsigned short unac_data2[];
extern unsigned short unac_data3[];
extern unsigned short unac_data4[];
extern unsigned short unac_data5[];
extern unsigned short unac_data6[];
extern unsigned short unac_data7[];
extern unsigned short unac_data8[];
extern unsigned short unac_data9[];
extern unsigned short unac_data10[];
extern unsigned short unac_data11[];
extern unsigned short unac_data12[];
extern unsigned short unac_data13[];
extern unsigned short unac_data14[];
extern unsigned short unac_data15[];
extern unsigned short unac_data16[];
extern unsigned short unac_data17[];
extern unsigned short unac_data18[];
extern unsigned short unac_data19[];
extern unsigned short unac_data20[];
extern unsigned short unac_data21[];
extern unsigned short unac_data22[];
extern unsigned short unac_data23[];
extern unsigned short unac_data24[];
extern unsigned short unac_data25[];
extern unsigned short unac_data26[];
extern unsigned short unac_data27[];
extern unsigned short unac_data28[];
extern unsigned short unac_data29[];
extern unsigned short unac_data30[];
extern unsigned short unac_data31[];
extern unsigned short unac_data32[];
extern unsigned short unac_data33[];
extern unsigned short unac_data34[];
extern unsigned short unac_data35[];
extern unsigned short unac_data36[];
extern unsigned short unac_data37[];
extern unsigned short unac_data38[];
extern unsigned short unac_data39[];
extern unsigned short unac_data40[];
extern unsigned short unac_data41[];
extern unsigned short unac_data42[];
extern unsigned short unac_data43[];
extern unsigned short unac_data44[];
extern unsigned short unac_data45[];
extern unsigned short unac_data46[];
extern unsigned short unac_data47[];
extern unsigned short unac_data48[];
extern unsigned short unac_data49[];
extern unsigned short unac_data50[];
extern unsigned short unac_data51[];
extern unsigned short unac_data52[];
extern unsigned short unac_data53[];
extern unsigned short unac_data54[];
extern unsigned short unac_data55[];
extern unsigned short unac_data56[];
extern unsigned short unac_data57[];
extern unsigned short unac_data58[];
extern unsigned short unac_data59[];
extern unsigned short unac_data60[];
extern unsigned short unac_data61[];
extern unsigned short unac_data62[];
extern unsigned short unac_data63[];
extern unsigned short unac_data64[];
extern unsigned short unac_data65[];
extern unsigned short unac_data66[];
extern unsigned short unac_data67[];
extern unsigned short unac_data68[];
extern unsigned short unac_data69[];
extern unsigned short unac_data70[];
extern unsigned short unac_data71[];
extern unsigned short unac_data72[];
extern unsigned short unac_data73[];
extern unsigned short unac_data74[];
extern unsigned short unac_data75[];
extern unsigned short unac_data76[];
extern unsigned short unac_data77[];
extern unsigned short unac_data78[];
extern unsigned short unac_data79[];
extern unsigned short unac_data80[];
extern unsigned short unac_data81[];
extern unsigned short unac_data82[];
extern unsigned short unac_data83[];
extern unsigned short unac_data84[];
extern unsigned short unac_data85[];
extern unsigned short unac_data86[];
extern unsigned short unac_data87[];
extern unsigned short unac_data88[];
extern unsigned short unac_data89[];
extern unsigned short unac_data90[];
extern unsigned short unac_data91[];
extern unsigned short unac_data92[];
extern unsigned short unac_data93[];
extern unsigned short unac_data94[];
extern unsigned short unac_data95[];
extern unsigned short unac_data96[];
extern unsigned short unac_data97[];
extern unsigned short unac_data98[];
extern unsigned short unac_data99[];
extern unsigned short unac_data100[];
extern unsigned short unac_data101[];
extern unsigned short unac_data102[];
extern unsigned short unac_data103[];
extern unsigned short unac_data104[];
extern unsigned short unac_data105[];
extern unsigned short unac_data106[];
extern unsigned short unac_data107[];
extern unsigned short unac_data108[];
extern unsigned short unac_data109[];
extern unsigned short unac_data110[];
extern unsigned short unac_data111[];
extern unsigned short unac_data112[];
extern unsigned short unac_data113[];
extern unsigned short unac_data114[];
extern unsigned short unac_data115[];
extern unsigned short unac_data116[];
extern unsigned short unac_data117[];
extern unsigned short unac_data118[];
extern unsigned short unac_data119[];
extern unsigned short unac_data120[];
extern unsigned short unac_data121[];
extern unsigned short unac_data122[];
extern unsigned short unac_data123[];
extern unsigned short unac_data124[];
extern unsigned short unac_data125[];
extern unsigned short unac_data126[];
extern unsigned short unac_data127[];
extern unsigned short unac_data128[];
extern unsigned short unac_data129[];
extern unsigned short unac_data130[];
extern unsigned short unac_data131[];
extern unsigned short unac_data132[];
extern unsigned short unac_data133[];
extern unsigned short unac_data134[];
extern unsigned short unac_data135[];
extern unsigned short unac_data136[];
extern unsigned short unac_data137[];
extern unsigned short unac_data138[];
extern unsigned short unac_data139[];
extern unsigned short unac_data140[];
extern unsigned short unac_data141[];
extern unsigned short unac_data142[];
extern unsigned short unac_data143[];
extern unsigned short unac_data144[];
extern unsigned short unac_data145[];
extern unsigned short unac_data146[];
extern unsigned short unac_data147[];
extern unsigned short unac_data148[];
extern unsigned short unac_data149[];
extern unsigned short unac_data150[];
extern unsigned short unac_data151[];
extern unsigned short unac_data152[];
extern unsigned short unac_data153[];
extern unsigned short unac_data154[];
extern unsigned short unac_data155[];
extern unsigned short unac_data156[];
extern unsigned short unac_data157[];
extern unsigned short unac_data158[];
extern unsigned short unac_data159[];
extern unsigned short unac_data160[];
extern unsigned short unac_data161[];
extern unsigned short unac_data162[];
extern unsigned short unac_data163[];
extern unsigned short unac_data164[];
extern unsigned short unac_data165[];
extern unsigned short unac_data166[];
extern unsigned short unac_data167[];
extern unsigned short unac_data168[];
extern unsigned short unac_data169[];
extern unsigned short unac_data170[];
extern unsigned short unac_data171[];
extern unsigned short unac_data172[];
extern unsigned short unac_data173[];
extern unsigned short unac_data174[];
extern unsigned short unac_data175[];
extern unsigned short unac_data176[];
extern unsigned short unac_data177[];
extern unsigned short unac_data178[];
extern unsigned short unac_data179[];
extern unsigned short unac_data180[];
extern unsigned short unac_data181[];
extern unsigned short unac_data182[];
extern unsigned short unac_data183[];
extern unsigned short unac_data184[];
extern unsigned short unac_data185[];
extern unsigned short unac_data186[];
extern unsigned short unac_data187[];
extern unsigned short unac_data188[];
extern unsigned short unac_data189[];
extern unsigned short unac_data190[];
extern unsigned short unac_data191[];
extern unsigned short unac_data192[];
extern unsigned short unac_data193[];
extern unsigned short unac_data194[];
extern unsigned short unac_data195[];
extern unsigned short unac_data196[];
extern unsigned short unac_data197[];
extern unsigned short unac_data198[];
extern unsigned short unac_data199[];
extern unsigned short unac_data200[];
extern unsigned short unac_data201[];
extern unsigned short unac_data202[];
extern unsigned short unac_data203[];
extern unsigned short unac_data204[];
extern unsigned short unac_data205[];
extern unsigned short unac_data206[];
extern unsigned short unac_data207[];
extern unsigned short unac_data208[];
extern unsigned short unac_data209[];
extern unsigned short unac_data210[];
extern unsigned short unac_data211[];
extern unsigned short unac_data212[];
extern unsigned short unac_data213[];
extern unsigned short unac_data214[];
extern unsigned short unac_data215[];
extern unsigned short unac_data216[];
extern unsigned short unac_data217[];
extern unsigned short unac_data218[];
extern unsigned short unac_data219[];
extern unsigned short unac_data220[];
extern unsigned short unac_data221[];
extern unsigned short unac_data222[];
extern unsigned short unac_data223[];
extern unsigned short unac_data224[];
extern unsigned short unac_data225[];
extern unsigned short unac_data226[];
extern unsigned short unac_data227[];
extern unsigned short unac_data228[];
extern unsigned short unac_data229[];
extern unsigned short unac_data230[];
extern unsigned short unac_data231[];
extern unsigned short unac_data232[];
extern unsigned short unac_data233[];
extern unsigned short unac_data234[];
extern unsigned short unac_data235[];
extern unsigned short unac_data236[];
extern unsigned short unac_data237[];
extern unsigned short unac_data238[];
extern unsigned short unac_data239[];
extern unsigned short unac_data240[];
extern unsigned short unac_data241[];
extern unsigned short unac_data242[];
extern unsigned short unac_data243[];
extern unsigned short unac_data244[];
extern unsigned short unac_data245[];
extern unsigned short unac_data246[];
extern unsigned short unac_data247[];
extern unsigned short unac_data248[];
extern unsigned short unac_data249[];
extern unsigned short unac_data250[];
extern unsigned short unac_data251[];
extern unsigned short unac_data252[];
extern unsigned short unac_data253[];
extern unsigned short unac_data254[];
extern unsigned short unac_data255[];
extern unsigned short unac_data256[];
extern unsigned short unac_data257[];
extern unsigned short unac_data258[];
extern unsigned short unac_data259[];
extern unsigned short unac_data260[];
extern unsigned short unac_data261[];
extern unsigned short unac_data262[];
extern unsigned short unac_data263[];
extern unsigned short unac_data264[];
extern unsigned short unac_data265[];
extern unsigned short unac_data266[];
extern unsigned short unac_data267[];
extern unsigned short unac_data268[];
extern unsigned short unac_data269[];
extern unsigned short unac_data270[];
extern unsigned short unac_data271[];
extern unsigned short unac_data272[];
extern unsigned short unac_data273[];
extern unsigned short unac_data274[];
extern unsigned short unac_data275[];
extern unsigned short unac_data276[];
extern unsigned short unac_data277[];
extern unsigned short unac_data278[];
extern unsigned short unac_data279[];
extern unsigned short unac_data280[];
extern unsigned short unac_data281[];
extern unsigned short unac_data282[];
extern unsigned short unac_data283[];
extern unsigned short unac_data284[];
extern unsigned short unac_data285[];
extern unsigned short unac_data286[];
extern unsigned short unac_data287[];
extern unsigned short unac_data288[];
extern unsigned short unac_data289[];
extern unsigned short unac_data290[];
extern unsigned short unac_data291[];
extern unsigned short unac_data292[];
extern unsigned short unac_data293[];
extern unsigned short unac_data294[];
extern unsigned short unac_data295[];
extern unsigned short unac_data296[];
extern unsigned short unac_data297[];
extern unsigned short unac_data298[];
extern unsigned short unac_data299[];
extern unsigned short unac_data300[];
extern unsigned short unac_data301[];
extern unsigned short unac_data302[];
extern unsigned short unac_data303[];
extern unsigned short unac_data304[];
extern unsigned short unac_data305[];
extern unsigned short unac_data306[];
extern unsigned short unac_data307[];
extern unsigned short unac_data308[];
extern unsigned short unac_data309[];
extern unsigned short unac_data310[];
extern unsigned short unac_data311[];
extern unsigned short unac_data312[];
extern unsigned short unac_data313[];
extern unsigned short unac_data314[];
extern unsigned short unac_data315[];
extern unsigned short unac_data316[];
extern unsigned short unac_data317[];
extern unsigned short unac_data318[];
extern unsigned short unac_data319[];
extern unsigned short unac_data320[];
extern unsigned short unac_data321[];
extern unsigned short unac_data322[];
extern unsigned short unac_data323[];
extern unsigned short unac_data324[];
extern unsigned short unac_data325[];
extern unsigned short unac_data326[];
extern unsigned short unac_data327[];
extern unsigned short unac_data328[];
extern unsigned short unac_data329[];
extern unsigned short unac_data330[];
extern unsigned short unac_data331[];
extern unsigned short unac_data332[];
extern unsigned short unac_data333[];
extern unsigned short unac_data334[];
extern unsigned short unac_data335[];
extern unsigned short unac_data336[];
extern unsigned short unac_data337[];
extern unsigned short unac_data338[];
extern unsigned short unac_data339[];
extern unsigned short unac_data340[];
extern unsigned short unac_data341[];
extern unsigned short unac_data342[];
extern unsigned short unac_data343[];
extern unsigned short unac_data344[];
extern unsigned short unac_data345[];
extern unsigned short unac_data346[];
extern unsigned short unac_data347[];
extern unsigned short unac_data348[];
extern unsigned short unac_data349[];
extern unsigned short unac_data350[];
extern unsigned short unac_data351[];
extern unsigned short unac_data352[];
extern unsigned short unac_data353[];
extern unsigned short unac_data354[];
extern unsigned short unac_data355[];
extern unsigned short unac_data356[];
extern unsigned short unac_data357[];
extern unsigned short unac_data358[];
extern unsigned short unac_data359[];
extern unsigned short unac_data360[];
extern unsigned short unac_data361[];
extern unsigned short unac_data362[];
extern unsigned short unac_data363[];
extern unsigned short unac_data364[];
extern unsigned short unac_data365[];
extern unsigned short unac_data366[];
extern unsigned short unac_data367[];
extern unsigned short unac_data368[];
extern unsigned short unac_data369[];
extern unsigned short unac_data370[];
extern unsigned short unac_data371[];
extern unsigned short unac_data372[];
extern unsigned short unac_data373[];
extern unsigned short unac_data374[];
extern unsigned short unac_data375[];
extern unsigned short unac_data376[];
extern unsigned short unac_data377[];
extern unsigned short unac_data378[];
extern unsigned short unac_data379[];
extern unsigned short unac_data380[];
extern unsigned short unac_data381[];
extern unsigned short unac_data382[];
extern unsigned short unac_data383[];
extern unsigned short unac_data384[];
extern unsigned short unac_data385[];
extern unsigned short unac_data386[];
extern unsigned short unac_data387[];
extern unsigned short unac_data388[];
extern unsigned short unac_data389[];
extern unsigned short unac_data390[];
extern unsigned short unac_data391[];
extern unsigned short unac_data392[];
extern unsigned short unac_data393[];
extern unsigned short unac_data394[];
extern unsigned short unac_data395[];
extern unsigned short unac_data396[];
extern unsigned short unac_data397[];
extern unsigned short unac_data398[];
extern unsigned short unac_data399[];
extern unsigned short unac_data400[];
extern unsigned short unac_data401[];
extern unsigned short unac_data402[];
extern unsigned short unac_data403[];
extern unsigned short unac_data404[];
extern unsigned short unac_data405[];
extern unsigned short unac_data406[];
extern unsigned short unac_data407[];
extern unsigned short unac_data408[];
extern unsigned short unac_data409[];
extern unsigned short unac_data410[];
extern unsigned short unac_data411[];
extern unsigned short unac_data412[];
extern unsigned short unac_data413[];
extern unsigned short unac_data414[];
extern unsigned short unac_data415[];
extern unsigned short unac_data416[];
extern unsigned short unac_data417[];
extern unsigned short unac_data418[];
extern unsigned short unac_data419[];
extern unsigned short unac_data420[];
extern unsigned short unac_data421[];
extern unsigned short unac_data422[];
extern unsigned short unac_data423[];
extern unsigned short unac_data424[];
extern unsigned short unac_data425[];
extern unsigned short unac_data426[];
extern unsigned short unac_data427[];
extern unsigned short unac_data428[];
extern unsigned short unac_data429[];
extern unsigned short unac_data430[];
extern unsigned short unac_data431[];
extern unsigned short unac_data432[];
extern unsigned short unac_data433[];
extern unsigned short unac_data434[];
extern unsigned short unac_data435[];
extern unsigned short unac_data436[];
extern unsigned short unac_data437[];
extern unsigned short unac_data438[];
extern unsigned short unac_data439[];
/* Generated by builder. Do not modify. End declarations */

#ifdef __cplusplus
}
#endif

#endif /* _unac_h */

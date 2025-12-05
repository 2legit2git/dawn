// dawn_block.h

#ifndef DAWN_BLOCK_H
#define DAWN_BLOCK_H

#include "dawn_types.h"
#include "dawn_md.h"

// #region Block Types

//! Block element types in the document model
typedef enum {
    BLOCK_PARAGRAPH,    //!< Regular text with inline formatting
    BLOCK_HEADER,       //!< H1-H6 header
    BLOCK_CODE,         //!< Fenced code block (```lang...```)
    BLOCK_MATH,         //!< Block math ($$...$$)
    BLOCK_TABLE,        //!< Markdown table
    BLOCK_IMAGE,        //!< Standalone image (![alt](path))
    BLOCK_HR,           //!< Horizontal rule (---, ***, ___)
    BLOCK_BLOCKQUOTE,   //!< Block quote (> prefix)
    BLOCK_LIST_ITEM,    //!< List item (-, *, +, 1.)
    BLOCK_FOOTNOTE_DEF, //!< Footnote definition ([^id]: content)
} BlockType;

// #endregion

// #region Inline Run Types

//! Inline run types for paragraph content
typedef enum {
    RUN_TEXT,           //!< Plain styled text
    RUN_LINK,           //!< Link [text](url)
    RUN_FOOTNOTE_REF,   //!< Footnote reference [^id]
    RUN_INLINE_MATH,    //!< Inline math $...$
    RUN_EMOJI,          //!< Emoji shortcode :name:
    RUN_HEADING_ID,     //!< Heading ID {#id}
} InlineRunType;

//! Inline run - a styled span within a paragraph
typedef struct {
    size_t byte_start;      //!< Start position in document
    size_t byte_end;        //!< End position (exclusive)
    MdStyle style;          //!< Combined style flags (MD_BOLD, MD_ITALIC, etc.)
    InlineRunType type;     //!< Type of run

    //! Type-specific data
    union {
        struct {
            size_t url_start;   //!< URL start position
            size_t url_len;     //!< URL length
        } link;

        struct {
            size_t id_start;    //!< Footnote ID start
            size_t id_len;      //!< Footnote ID length
        } footnote;

        struct {
            size_t content_start; //!< Math content start
            size_t content_len;   //!< Math content length
        } math;

        struct {
            const char *emoji;  //!< Resolved emoji string (pointer to static data)
        } emoji;

        struct {
            size_t id_start;    //!< ID start position
            size_t id_len;      //!< ID length
        } heading_id;
    } data;
} InlineRun;

// #endregion

// #region Block Structure

//! Block - a top-level document element
typedef struct Block {
    BlockType type;         //!< Block type

    // Position in document (byte offsets into GapBuffer)
    size_t start;           //!< First byte of block
    size_t end;             //!< Last byte + 1 (exclusive)

    // Cached virtual row info
    int vrow_start;         //!< Virtual row where block starts
    int vrow_count;         //!< Number of virtual rows this block occupies

    //! Type-specific data
    union {
        //! BLOCK_HEADER data
        struct {
            int level;              //!< Header level (1-6)
            size_t content_start;   //!< Content start (after "# ")
            size_t id_start;        //!< Heading ID start, or 0 if none
            size_t id_len;          //!< Heading ID length
        } header;

        //! BLOCK_CODE data
        struct {
            size_t lang_start;      //!< Language name start
            size_t lang_len;        //!< Language name length
            size_t content_start;   //!< Code content start
            size_t content_len;     //!< Code content length
            char *highlighted;      //!< Cached highlighted output (NULL if not computed)
            size_t highlighted_len; //!< Length of highlighted output
        } code;

        //! BLOCK_MATH data
        struct {
            size_t content_start;   //!< LaTeX content start
            size_t content_len;     //!< LaTeX content length
            void *tex_sketch;       //!< TexSketch* cached render (NULL if not computed)
        } math;

        //! BLOCK_TABLE data
        struct {
            int col_count;          //!< Number of columns
            int row_count;          //!< Number of rows (including header)
            MdAlign align[MD_TABLE_MAX_COLS]; //!< Column alignments
        } table;

        //! BLOCK_IMAGE data
        struct {
            size_t alt_start;       //!< Alt text start
            size_t alt_len;         //!< Alt text length
            size_t path_start;      //!< Image path start
            size_t path_len;        //!< Image path length
            int width;              //!< Parsed width (negative = percentage)
            int height;             //!< Parsed height (negative = percentage)
            int display_rows;       //!< Calculated rows
            char *resolved_path;    //!< Cached resolved path (NULL if not computed)
        } image;

        //! BLOCK_HR data
        struct {
            size_t rule_len;        //!< Length of rule syntax
        } hr;

        //! BLOCK_BLOCKQUOTE data
        struct {
            int level;              //!< Nesting level (1 = >, 2 = >>, etc.)
            size_t content_start;   //!< Content start (after "> ")
        } quote;

        //! BLOCK_LIST_ITEM data
        struct {
            int list_type;          //!< 1 = unordered, 2 = ordered
            int indent;             //!< Leading space count
            int task_state;         //!< 0 = not task, 1 = unchecked [ ], 2 = checked [x]
            size_t content_start;   //!< Content start
        } list;

        //! BLOCK_PARAGRAPH data
        struct {
            InlineRun *runs;        //!< Array of inline runs
            int run_count;          //!< Number of runs
            int run_capacity;       //!< Allocated capacity
        } paragraph;

        //! BLOCK_FOOTNOTE_DEF data
        struct {
            size_t id_start;        //!< Footnote ID start
            size_t id_len;          //!< Footnote ID length
            size_t content_start;   //!< Definition content start
        } footnote;
    } data;
} Block;

// #endregion

// #region Block Cache

//! Initial block array capacity
#define BLOCK_CACHE_INITIAL_CAPACITY 64

//! Block cache - the parsed document model
typedef struct {
    Block *blocks;          //!< Array of blocks
    int count;              //!< Number of blocks
    int capacity;           //!< Allocated capacity

    // Document metadata
    size_t text_len;        //!< Document length when parsed
    int total_vrows;        //!< Total virtual rows

    // Cache validity
    bool valid;             //!< Cache is valid
    int wrap_width;         //!< Text width used for vrow calculation
    int text_height;        //!< Text area height for image scaling
} BlockCache;

// #endregion

// #region Block Cache API

//! Initialize a block cache
//! @param bc block cache to initialize
void block_cache_init(BlockCache *bc);

//! Free a block cache and all contained resources
//! @param bc block cache to free
void block_cache_free(BlockCache *bc);

//! Parse entire document into blocks
//! @param bc block cache to populate
//! @param gb gap buffer containing document text
//! @param wrap_width text width for wrapping calculations
//! @param text_height text area height for image scaling
void block_cache_parse(BlockCache *bc, const GapBuffer *gb, int wrap_width, int text_height);

//! Invalidate the cache (mark for reparse)
//! @param bc block cache to invalidate
void block_cache_invalidate(BlockCache *bc);

// #endregion

// #region Block Query API

//! Find block containing a byte position (binary search)
//! @param bc block cache
//! @param byte_pos byte position to find
//! @return pointer to block, or NULL if not found
Block *block_at_pos(BlockCache *bc, size_t byte_pos);

//! Find block containing a virtual row (binary search)
//! @param bc block cache
//! @param vrow virtual row to find
//! @return pointer to block, or NULL if not found
Block *block_at_vrow(BlockCache *bc, int vrow);

//! Get index of block containing a byte position
//! @param bc block cache
//! @param byte_pos byte position to find
//! @return block index, or -1 if not found
int block_index_at_pos(BlockCache *bc, size_t byte_pos);

//! Calculate cursor virtual row within a single block
//! @param block the block containing the cursor
//! @param gb gap buffer
//! @param cursor cursor position
//! @param wrap_width text width for wrapping
//! @return virtual row offset from block's vrow_start
int calc_cursor_vrow_in_block(const Block *block, const GapBuffer *gb,
                              size_t cursor, int wrap_width);

// #endregion

// #region Inline Run API

//! Parse inline runs for a paragraph block (called during block parsing)
//! @param block paragraph block to parse
//! @param gb gap buffer
void block_parse_inline_runs(Block *block, const GapBuffer *gb);

//! Free inline runs for a paragraph block
//! @param block paragraph block
void block_free_inline_runs(Block *block);

// #endregion

#endif // DAWN_BLOCK_H

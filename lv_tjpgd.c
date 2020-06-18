/**
 * @file lv_tjpgd.c
 *
 * LOG:
 * - 13/Jun/2020: Working when allocating memory for the whole
 *                decoded image.
 *
 * TODO:
 * - Improve/cleanup/comment code.
 * - Try to understand how can we control the amount of pixels we get back from the decoder.
 * - LVGL FS abstraction layer.
 * - Work when using the read_line callback.
 * - Work on images stored as c arrays and not as files.
 */

/*********************
 *      INCLUDES
 *********************/
#include "lvgl/lvgl.h"

#include <stdlib.h>
#include <stdio.h>

#include "lv_tjpgd.h"
#include "tjpgd.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static lv_res_t decoder_info(struct _lv_img_decoder * decoder, const void * src, lv_img_header_t * header);
static lv_res_t decoder_open(lv_img_decoder_t * dec, lv_img_decoder_dsc_t * dsc);
static lv_res_t decoder_read(lv_img_decoder_t * decoder,
                         lv_img_decoder_dsc_t * dsc,
                         lv_coord_t x, lv_coord_t y,
                         lv_coord_t len, uint8_t *buf);
static void decoder_close(lv_img_decoder_t * dec, lv_img_decoder_dsc_t * dsc);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

 /* File height and wide */
typedef struct {
    uint16_t height;
    uint16_t width;
} img_size;

 /* User defined device identifier */
typedef struct {
    FILE        *fp;                /* File pointer for input function */
    uint8_t     *frame_buffer;      /* Pointer to the frame buffer for output function */
    void        *work;              /* Work buffer */
    uint32_t    out_func_calls;     /* NOTE: Keep track on the amount of times the decoder calls the output callbacks */
    uint16_t    frame_buffer_width; /* Width of the frame buffer [pix] */
    img_size    size;
    JRECT       last_decoded_coord;
    uint8_t     *lvgl_feeder_ptr;
    size_t      *after_decode_pos;
    int32_t      fp_consumed_bytes;
} decoding_ctx_t;

// See: JD_SZBUF		512	/* Size of stream input buffer */
#define TJPGD_WORK_BUFFER_SIZE  (20*1024)

/* Decoding session */
JDEC jdec;

/* Device ID */
static decoding_ctx_t user_ctx;

#define VALID_FILE_EXTENSION    "jpg"

/* Feed decoder callback
 *
 * @param jd: Device identifier for the session (5th argument of jd_prepare function)
 * @param buff: buffer to store data read
 * @param nbyte: (buff != NULL) bytes to read from input stream
 *               (buff == NULL) bytes to remove from input stream
 *
 * @retval number of bytes successfully read
 */
static uint16_t on_feed_decoder_cb(JDEC* jd, uint8_t* buff, uint16_t nbyte);

/* Decoder output callback.
 *
 * @param jd:
 * @param bitmap:
 * @param rect:
 *
 * @retval 1 Continue to decompress, 0 to abort.
 */
static uint16_t on_decoder_output_cb(JDEC* jd, void* bitmap, JRECT* rect);
static uint16_t on_decoder_line_output_cb(JDEC* jd, void* bitmap, JRECT* rect);

/**
 * Register the JPG decoder functions in LVGL
 */
void lv_tjpgd_init(void)
{
    /* Allocate work area for tjpgd */
    user_ctx.work = malloc(TJPGD_WORK_BUFFER_SIZE);

    lv_img_decoder_t * dec = lv_img_decoder_create();

    /* Get information about the image */
    lv_img_decoder_set_info_cb(dec, decoder_info);
    /* Open the image, either store the decoded
     * image or set it to NULL to indicate
     * the image can be read line-by-line */
    lv_img_decoder_set_open_cb(dec, decoder_open);
    /* If open didn't fully open the image this
     * function should give some decoded data
     * (max 1 line) from a given position */
    lv_img_decoder_set_read_line_cb(dec, decoder_read);
    /* Close the opened image, free, the allocated
     * resources*/
    lv_img_decoder_set_close_cb(dec, decoder_close);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Get information about a JPG image
 *
 * @param src can be file name or pointer to a C array
 * @param header store the info here
 * @return LV_RES_OK: no error; LV_RES_INV: can't get the info
 */
static lv_res_t decoder_info(struct _lv_img_decoder * decoder, const void * src, lv_img_header_t * header)
{
    (void) decoder; /*Unused*/
     lv_img_src_t src_type = lv_img_src_get_type(src);          /*Get the source type*/

     /*If it's a JPG file...*/
     if(src_type == LV_IMG_SRC_FILE) {
         const char * fn = src;

         /*Check the extension*/
         if(!strcmp(&fn[strlen(fn) - 3], VALID_FILE_EXTENSION)) {

             user_ctx.fp = fopen(fn, "rb");
             if(!user_ctx.fp) {
                return LV_RES_INV;
             }

             /* Prepare for decompress and get the image information */
             JRESULT res = jd_prepare(&jdec, on_feed_decoder_cb, user_ctx.work, TJPGD_WORK_BUFFER_SIZE, &user_ctx);

            if (JDR_OK == res) {
                header->always_zero = 0;
                /* Color format */
                header->cf = LV_IMG_CF_RAW; // LV_IMG_CF_RAW

                /* Image size:
                 * When letting LVGL know what's the size of the image we also need
                 * to consider the scaling factor.
                 * If the image is 200 * 200 px, when setting an scaling factor
                 * 1:2 then the image ends up being 100 * 100px.
                 * That is why we divide the information got from jd_prepare by
                 * a divisor depending on the scaling factor.
                 *
                 * See lv_tjpgd.h */
                header->w = (lv_coord_t) jdec.width / LV_TJPGD_SCALING_FACTOR_DIV;
                // header->h = (lv_coord_t) jdec.height / LV_TJPGD_SCALING_FACTOR_DIV;
                header->h = 8;

                user_ctx.frame_buffer_width = jdec.width / LV_TJPGD_SCALING_FACTOR_DIV;
                user_ctx.size.height = header->h;
                user_ctx.size.width = header->w;

                return LV_RES_OK;
            } else {
                printf("Error ID: %d", (int) res);
                return LV_RES_INV;
            }
         }
     }
     /*If it's a file in a  C array...*/
     else if(src_type == LV_IMG_SRC_VARIABLE) {

        /* TODO */

        return LV_RES_OK;
     }

     return LV_RES_INV;
}

/**
 * Open a JPG image and return the decoded image.
 * if dsc->img_data = NULL; the read_line callback will be called
 *
 * @param src can be file name or pointer to a C array
 * @param style style of the image object (unused now but certain formats might use it)
 * @return pointer to the decoded image or  `LV_IMG_DECODER_OPEN_FAIL` if failed
 */
static lv_res_t decoder_open(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{
    (void) decoder;

    lv_res_t retval = LV_RES_INV;

    /*If it's a JPG file...*/
    if(dsc->src_type == LV_IMG_SRC_FILE) {
        const char * fn = dsc->src;

        if(!strcmp(&fn[strlen(fn) - 3], VALID_FILE_EXTENSION)) {

#if 0   /* Lets try decoding the image in chunks ;) */
            JRESULT error = JDR_OK;

            /* NOTE: Allocate memory for the whole decoded image. */
            uint32_t decoded_image_buffer_size = user_ctx.size.width * user_ctx.size.height * BYTES_ON_PIXEL;
            user_ctx.frame_buffer = (uint8_t *) malloc(decoded_image_buffer_size);

            if (!user_ctx.frame_buffer) {
                return LV_RES_INV;
            }

            /* NOTE
             * This is the first implementation of the JPG decoder support.
             * As we are testing this on a PC almost any amount of dinamically allocated
             * memory is available for us.
             *
             * We can not make this assumption when using microcontrollers, in such case
             * we should decode the image in chunks. When decoding the image in chunks
             * we most surely will need to set dsc->img_data to NULL, then the LVGL image
             * decoder will call the read callback. */
            error = jd_decomp(&jdec, on_decoder_output_cb, LV_TJPGD_SCALING_FACTOR);

            if (JDR_OK != error) {
                printf("Error ID: %d", (int) error);
                retval = LV_RES_INV;
            } else {
                dsc->img_data = user_ctx.frame_buffer;
                retval = LV_RES_OK;
            }

            return retval;
#else
            dsc->img_data = NULL;
            retval = LV_RES_OK;

            return retval;
        }
#endif

    }
    /*If it's a JPG file in a  C array...*/
    else if(dsc->src_type == LV_IMG_SRC_VARIABLE) {

        /* TODO */

        return LV_RES_OK;     /*Return with its pointer*/
    }

    return LV_RES_INV;    /*If not returned earlier then it failed*/
}

/**
 * Decode `len` pixels starting from the given `x`, `y` coordinates and store them in `buf`.
 * Required only if the "open" function can't open the whole decoded pixel array. (dsc->img_data == NULL)
 *
 * @param decoder pointer to the decoder the function associated with
 * @param dsc pointer to decoder descriptor
 * @param x start x coordinate
 * @param y start y coordinate
 * @param len number of pixels to decode
 * @param buf a buffer to store the decoded pixels
 * @return LV_RES_OK: ok; LV_RES_INV: failed
 */
static lv_res_t decoder_read(lv_img_decoder_t * decoder,
                             lv_img_decoder_dsc_t * dsc,
                             lv_coord_t x, lv_coord_t y,
                             lv_coord_t len, uint8_t *buf)
{
    lv_res_t retval = LV_RES_INV;

    static bool buffered_data = false;
    static uint8_t lines_sent = 0;

    if (buffered_data) {
        memcpy((void *) buf, (void *) user_ctx.lvgl_feeder_ptr, len * BYTES_ON_PIXEL);
        user_ctx.lvgl_feeder_ptr += (len * BYTES_ON_PIXEL);
        retval = LV_RES_OK;
        lines_sent++;

        if (7 < lines_sent) {
            buffered_data = false;
            lines_sent = 0;
            free(user_ctx.frame_buffer);
            user_ctx.frame_buffer = NULL;
        }

    } else {
        /* We do not have the requested pixels, lets decode them! */
        JRESULT error = JDR_OK;

        /* Allocate memory to store the decoded data */
        uint32_t decoded_image_buffer_size = user_ctx.size.width * 8 * BYTES_ON_PIXEL;
        user_ctx.frame_buffer = (uint8_t *) malloc(decoded_image_buffer_size);

        size_t before_decompression = ftell(user_ctx.fp);

        jdec.height = 8;
        error = jd_decomp(&jdec, on_decoder_line_output_cb, LV_TJPGD_SCALING_FACTOR);

        size_t bytes_to_send = len * BYTES_ON_PIXEL;
        memcpy((void *) buf, (void *) user_ctx.frame_buffer, bytes_to_send);
        lines_sent++;
        user_ctx.lvgl_feeder_ptr = user_ctx.frame_buffer + bytes_to_send;
        retval = LV_RES_OK;

        user_ctx.after_decode_pos = ftell(user_ctx.fp);
        buffered_data = true;
    }

    return retval;
}

/**
 * Free the allocated resources
 */
static void decoder_close(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{
    (void) decoder; /*Unused*/

    free(user_ctx.work);

    if(dsc->img_data) {
        free((uint8_t *) dsc->img_data);
    }
}

/* Feed decoder callback
 *
 * @param jd: Device identifier for the session (5th argument of jd_prepare function)
 * @param buff: buffer to store data read
 * @param nbyte: (buff != NULL) bytes to read from input stream
 *               (buff == NULL) bytes to remove from input stream
 *
 * @retval number of bytes successfully read
 */
static uint16_t on_feed_decoder_cb(JDEC* jd, uint8_t* buff, uint16_t nbyte)
{
    uint16_t retval = 0;

    decoding_ctx_t *ctx = (decoding_ctx_t *) jd->device;
    ctx->fp_consumed_bytes += nbyte;

    if (buff) {
        retval = fread(buff, 1, nbyte, ctx->fp);
    } else {
        retval = fseek(ctx->fp, nbyte, SEEK_CUR) ? 0 : nbyte;
    }

    return retval;
}

/* Decoder output callback
 *
 * @param jd:
 * @param bitmap:
 * @param rect:
 *
 * @retval 1 Continue to decompress, 0 to abort.
 */
static uint16_t on_decoder_output_cb(JDEC* jd, void* bitmap, JRECT* rect)
{
    decoding_ctx_t *ctx = (decoding_ctx_t*) jd->device;

    ctx->out_func_calls++;

    /* Copy decompressed RGB rectangular to the frame buffer */
    uint8_t *src = (uint8_t *) bitmap;

    /* Were in the framebuffer we are writing the bitmap data */
    uint8_t *dst = ctx->frame_buffer + (BYTES_ON_PIXEL * ((rect->top * ctx->frame_buffer_width) + rect->left));
    uint16_t bws = BYTES_ON_PIXEL * (rect->right - rect->left + 1); /* Width of source rectangular */
    uint16_t bwd = BYTES_ON_PIXEL * ctx->frame_buffer_width; /* Width of frame buffer */

    for (uint16_t y = rect->top; y <= rect->bottom; y++) {
        memcpy(dst, src, bws); /* Copy a line */
        src += bws; /* Next line */
        dst += bwd;
    }

    return 1;
}

static uint16_t on_decoder_line_output_cb(JDEC* jd, void* bitmap, JRECT* rect)
{
    decoding_ctx_t *ctx = (decoding_ctx_t*) jd->device;

    ctx->out_func_calls++;
    /* Keep track of the last decoded coordinate */
    ctx->last_decoded_coord.bottom  = rect->bottom;
    ctx->last_decoded_coord.top     = rect->top;
    ctx->last_decoded_coord.left    = rect->left;
    ctx->last_decoded_coord.right   = rect->right;

    /* Copy decompressed RGB rectangular to the frame buffer */
    uint8_t *src = (uint8_t *) bitmap;

    /* Were in the framebuffer we are writing the bitmap data */
    uint8_t *dst = ctx->frame_buffer + (BYTES_ON_PIXEL * ((rect->top * ctx->frame_buffer_width) + rect->left));
    uint16_t bws = BYTES_ON_PIXEL * (rect->right - rect->left + 1); /* Width of source rectangular */
    uint16_t bwd = BYTES_ON_PIXEL * ctx->frame_buffer_width; /* Width of frame buffer */

    for (uint16_t y = rect->top; y <= rect->bottom; y++) {
        memcpy(dst, src, bws); /* Copy a line */
        src += bws; /* Next line */
        dst += bwd;
    }

    return 1;
}

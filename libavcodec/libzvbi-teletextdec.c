/*
 * Teletext decoding for ffmpeg
 * Copyright (c) 2005-2010, 2012 Wolfram Gloger
 * Copyright (c) 2013 Marton Balint
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "avcodec.h"
#include "libavcodec/ass.h"
#include "libavutil/opt.h"
#include "libavutil/bprint.h"
#include "libavutil/intreadwrite.h"

#include <libzvbi.h>

#define TEXT_MAXSZ    (25 * (56 + 1) * 4 + 2)
#define VBI_NB_COLORS 40
#define RGBA(r,g,b,a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define VBI_R(rgba)   (((rgba) >> 0) & 0xFF)
#define VBI_G(rgba)   (((rgba) >> 8) & 0xFF)
#define VBI_B(rgba)   (((rgba) >> 16) & 0xFF)
#define VBI_A(rgba)   (((rgba) >> 24) & 0xFF)
#define MAX_BUFFERED_PAGES 25

typedef struct TeletextPage
{
    AVSubtitleRect *sub_rect;
    int pgno;
    int subno;
    int64_t pts;
} TeletextPage;

/* main data structure */
typedef struct TeletextContext
{
    AVClass        *class;
    char           *pgno;
    int             x_offset;
    int             y_offset;
    int             format_id; /* 0 = bitmap, 1 = text/ass */
    int             chop_top;
    int             sub_duration; /* in msec */
    int             transparent_bg;
    int             chop_spaces;

    int             lines_processed;
    TeletextPage    *pages;
    int             nb_pages;
    int64_t         pts;
    int             handler_ret;

    vbi_decoder *   vbi;
    vbi_dvb_demux * dx;
#ifdef DEBUG
    vbi_export *    ex;
#endif
    /* Don't even _think_ about making sliced stack-local! */
    vbi_sliced      sliced[64];
} TeletextContext;

/************************************************************************/

static int
chop_spaces_utf8(const unsigned char* t, int len)
{
    t += len;
    while (len > 0) {
        if (*--t != ' ' || (len-1 > 0 && *(t-1) & 0x80))
            break;
        --len;
    }
    return len;
}

static void
subtitle_rect_free(AVSubtitleRect **sub_rect)
{
    av_freep(&(*sub_rect)->pict.data[0]);
    av_freep(&(*sub_rect)->pict.data[1]);
    av_freep(&(*sub_rect)->ass);
    av_freep(sub_rect);
}

static int
create_ass_text(TeletextContext *ctx, const char *text, char **ass)
{
    int ret;
    AVBPrint buf, buf2;
    const int ts_start    = av_rescale_q(ctx->pts,          AV_TIME_BASE_Q,        (AVRational){1, 100});
    const int ts_duration = av_rescale_q(ctx->sub_duration, (AVRational){1, 1000}, (AVRational){1, 100});

    /* First we escape the plain text into buf. */
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);
    ff_ass_bprint_text_event(&buf, text, strlen(text), "", 0);

    if (!av_bprint_is_complete(&buf)) {
        av_bprint_finalize(&buf, NULL);
        return AVERROR(ENOMEM);
    }

    /* Then we create the ass dialog line in buf2 from the escaped text in buf. */
    av_bprint_init(&buf2, 0, AV_BPRINT_SIZE_UNLIMITED);
    ff_ass_bprint_dialog(&buf2, buf.str, ts_start, ts_duration, 0);
    av_bprint_finalize(&buf, NULL);

    if (!av_bprint_is_complete(&buf2)) {
        av_bprint_finalize(&buf2, NULL);
        return AVERROR(ENOMEM);
    }

    if ((ret = av_bprint_finalize(&buf2, ass)) < 0)
        return ret;

    return 0;
}

// draw a page as text
static int
gen_sub_text(TeletextContext *ctx, AVSubtitleRect *sub_rect, vbi_page *page, int chop_top)
{
    const char *in;
    AVBPrint buf;
    char *vbi_text = av_malloc(TEXT_MAXSZ);
    int sz;

    if (!vbi_text)
        return AVERROR(ENOMEM);

    sz = vbi_print_page_region(page, vbi_text, TEXT_MAXSZ-1, "UTF-8",
                                   /*table mode*/ TRUE, FALSE,
                                   0,             chop_top,
                                   page->columns, page->rows-chop_top);
    if (sz <= 0) {
        av_log(ctx, AV_LOG_ERROR, "vbi_print error\n");
        av_free(vbi_text);
        return AVERROR_EXTERNAL;
    }
    vbi_text[sz] = '\0';
    in  = vbi_text;
    av_bprint_init(&buf, 0, TEXT_MAXSZ);

    if (ctx->chop_spaces) {
        for (;;) {
            int nl, sz;

            // skip leading spaces and newlines
            in += strspn(in, " \n");
            // compute end of row
            for (nl = 0; in[nl]; ++nl)
                if (in[nl] == '\n' && (nl==0 || !(in[nl-1] & 0x80)))
                    break;
            if (!in[nl])
                break;
            // skip trailing spaces
            sz = chop_spaces_utf8(in, nl);
            av_bprint_append_data(&buf, in, sz);
            av_bprintf(&buf, "\n");
            in += nl;
        }
    } else {
        av_bprintf(&buf, "%s\n", vbi_text);
    }
    av_free(vbi_text);

    if (!av_bprint_is_complete(&buf)) {
        av_bprint_finalize(&buf, NULL);
        return AVERROR(ENOMEM);
    }

    if (buf.len) {
        int ret;
        sub_rect->type = SUBTITLE_ASS;
        if ((ret = create_ass_text(ctx, buf.str, &sub_rect->ass)) < 0) {
            av_bprint_finalize(&buf, NULL);
            return ret;
        }
        av_log(ctx, AV_LOG_DEBUG, "subtext:%s:txetbus\n", sub_rect->ass);
    } else {
        sub_rect->type = SUBTITLE_NONE;
    }
    av_bprint_finalize(&buf, NULL);
    return 0;
}

static void
fix_transparency(TeletextContext *ctx, AVSubtitleRect *sub_rect, vbi_page *page, int chop_top, uint8_t transparent_color, int resx, int resy)
{
    int iy;

    // Hack for transparency, inspired by VLC code...
    for (iy = 0; iy < resy; iy++) {
        uint8_t *pixel = sub_rect->pict.data[0] + iy * sub_rect->pict.linesize[0];
        vbi_char *vc = page->text + (iy / 10 + chop_top) * page->columns;
        vbi_char *vcnext = vc + page->columns;
        for (; vc < vcnext; vc++) {
            uint8_t *pixelnext = pixel + 12;
            switch (vc->opacity) {
                case VBI_TRANSPARENT_SPACE:
                    memset(pixel, transparent_color, 12);
                    break;
                case VBI_OPAQUE:
                case VBI_SEMI_TRANSPARENT:
                    if (!ctx->transparent_bg)
                        break;
                case VBI_TRANSPARENT_FULL:
                    for(; pixel < pixelnext; pixel++)
                        if (*pixel == vc->background)
                            *pixel = transparent_color;
                    break;
            }
            pixel = pixelnext;
        }
    }
}

// draw a page as bitmap
static int
gen_sub_bitmap(TeletextContext *ctx, AVSubtitleRect *sub_rect, vbi_page *page, int chop_top)
{
    int resx = page->columns * 12;
    int resy = (page->rows - chop_top) * 10;
    uint8_t ci, cmax = 0;
    int ret;
    vbi_char *vc = page->text + (chop_top * page->columns);
    vbi_char *vcend = page->text + (page->rows * page->columns);

    for (; vc < vcend; vc++) {
        if (vc->opacity != VBI_TRANSPARENT_SPACE) {
            cmax = VBI_NB_COLORS;
            break;
        }
    }

    if (cmax == 0) {
        av_log(ctx, AV_LOG_DEBUG, "dropping empty page %3x\n", page->pgno);
        sub_rect->type = SUBTITLE_NONE;
        return 0;
    }

    if ((ret = avpicture_alloc(&sub_rect->pict, AV_PIX_FMT_PAL8, resx, resy)) < 0)
        return ret;
    // Yes, we want to allocate the palette on our own because AVSubtitle works this way
    sub_rect->pict.data[1] = NULL;

    vbi_draw_vt_page_region(page, VBI_PIXFMT_PAL8,
                            sub_rect->pict.data[0], sub_rect->pict.linesize[0],
                            0, chop_top, page->columns, page->rows - chop_top,
                            /*reveal*/ 1, /*flash*/ 1);

    fix_transparency(ctx, sub_rect, page, chop_top, cmax, resx, resy);
    sub_rect->x = ctx->x_offset;
    sub_rect->y = ctx->y_offset;
    sub_rect->w = resx;
    sub_rect->h = resy;
    sub_rect->nb_colors = (int)cmax + 1;
    sub_rect->pict.data[1] = av_mallocz(AVPALETTE_SIZE);
    if (!sub_rect->pict.data[1]) {
        av_freep(&sub_rect->pict.data[0]);
        return AVERROR(ENOMEM);
    }
    for (ci = 0; ci < cmax; ci++) {
        int r, g, b, a;

        r = VBI_R(page->color_map[ci]);
        g = VBI_G(page->color_map[ci]);
        b = VBI_B(page->color_map[ci]);
        a = VBI_A(page->color_map[ci]);
        ((uint32_t *)sub_rect->pict.data[1])[ci] = RGBA(r, g, b, a);
#ifdef DEBUG
        av_log(ctx, AV_LOG_DEBUG, "palette %0x\n",
               ((uint32_t *)sub_rect->pict.data[1])[ci]);
#endif
    }
    ((uint32_t *)sub_rect->pict.data[1])[cmax] = RGBA(0, 0, 0, 0);
    sub_rect->type = SUBTITLE_BITMAP;
    return 0;
}

static void
handler(vbi_event *ev, void *user_data)
{
    TeletextContext *ctx = user_data;
    TeletextPage *new_pages;
    vbi_page page;
    int res;
    char pgno_str[12];
    vbi_subno subno;
    vbi_page_type vpt;
    int chop_top;
    char *lang;

    snprintf(pgno_str, sizeof pgno_str, "%03x", ev->ev.ttx_page.pgno);
    av_log(ctx, AV_LOG_DEBUG, "decoded page %s.%02x\n",
           pgno_str, ev->ev.ttx_page.subno & 0xFF);

    if (strcmp(ctx->pgno, "*") && !strstr(ctx->pgno, pgno_str))
        return;
    if (ctx->handler_ret < 0)
        return;

    /* Fetch the page.  */
    res = vbi_fetch_vt_page(ctx->vbi, &page,
                            ev->ev.ttx_page.pgno,
                            ev->ev.ttx_page.subno,
                            VBI_WST_LEVEL_3p5, 25, TRUE);

    if (!res)
        return;

#ifdef DEBUG
    fprintf(stderr, "\nSaving res=%d dy0=%d dy1=%d...\n",
            res, page.dirty.y0, page.dirty.y1);
    fflush(stderr);

    if (!vbi_export_stdio(ctx->ex, stderr, &page))
        fprintf(stderr, "failed: %s\n", vbi_export_errstr(ctx->ex));
#endif

    vpt = vbi_classify_page(ctx->vbi, ev->ev.ttx_page.pgno, &subno, &lang);
    chop_top = ctx->chop_top ||
        ((page.rows > 1) && (vpt == VBI_SUBTITLE_PAGE));

    av_log(ctx, AV_LOG_DEBUG, "%d x %d page chop:%d\n",
           page.columns, page.rows, chop_top);

    if (ctx->nb_pages < MAX_BUFFERED_PAGES) {
        if ((new_pages = av_realloc_array(ctx->pages, ctx->nb_pages + 1, sizeof(TeletextPage)))) {
            TeletextPage *cur_page = new_pages + ctx->nb_pages;
            ctx->pages = new_pages;
            cur_page->sub_rect = av_mallocz(sizeof(*cur_page->sub_rect));
            cur_page->pts = ctx->pts;
            cur_page->pgno = ev->ev.ttx_page.pgno;
            cur_page->subno = ev->ev.ttx_page.subno;
            if (cur_page->sub_rect) {
                res = (ctx->format_id == 0) ?
                    gen_sub_bitmap(ctx, cur_page->sub_rect, &page, chop_top) :
                    gen_sub_text  (ctx, cur_page->sub_rect, &page, chop_top);
                if (res < 0) {
                    av_freep(&cur_page->sub_rect);
                    ctx->handler_ret = res;
                } else {
                    ctx->pages[ctx->nb_pages++] = *cur_page;
                }
            } else {
                ctx->handler_ret = AVERROR(ENOMEM);
            }
        } else {
            ctx->handler_ret = AVERROR(ENOMEM);
        }
    } else {
        //TODO: If multiple packets contain more than one page, pages may got queued up, and this may happen...
        av_log(ctx, AV_LOG_ERROR, "Buffered too many pages, dropping page %s.\n", pgno_str);
        ctx->handler_ret = AVERROR(ENOSYS);
    }

    vbi_unref_page(&page);
}

static int
teletext_decode_frame(AVCodecContext *avctx,
                      void *data, int *data_size,
                      AVPacket *pkt)
{
    TeletextContext *ctx = avctx->priv_data;
    AVSubtitle      *sub = data;
    const uint8_t   *buf = pkt->data;
    int             left = pkt->size;
    uint8_t         pesheader[45] = {0x00, 0x00, 0x01, 0xbd, 0x00, 0x00, 0x85, 0x80, 0x24, 0x21, 0x00, 0x01, 0x00, 0x01};
    int             pesheader_size = sizeof(pesheader);
    const uint8_t   *pesheader_buf = pesheader;
    int             ret = 0;

    if (!ctx->vbi) {
        if (!(ctx->vbi = vbi_decoder_new()))
            return AVERROR(ENOMEM);
        if (!vbi_event_handler_add(ctx->vbi, VBI_EVENT_TTX_PAGE, handler, ctx)) {
            vbi_decoder_delete(ctx->vbi);
            ctx->vbi = NULL;
            return AVERROR(ENOMEM);
        }
    }
    if (!ctx->dx && (!(ctx->dx = vbi_dvb_pes_demux_new (/* callback */ NULL, NULL))))
        return AVERROR(ENOMEM);

    if (avctx->pkt_timebase.den && pkt->pts != AV_NOPTS_VALUE)
        ctx->pts = av_rescale_q(pkt->pts, avctx->pkt_timebase, AV_TIME_BASE_Q);

    if (left) {
        // We allow unreasonably big packets, even if the standard only allows a max size of 1472
        if ((pesheader_size + left) < 184 || (pesheader_size + left) > 65504 || (pesheader_size + left) % 184 != 0)
            return AVERROR_INVALIDDATA;

        memset(pesheader + 14, 0xff, pesheader_size - 14);
        AV_WB16(pesheader + 4, left + pesheader_size - 6);

        /* PTS is deliberately left as 0 in the PES header, otherwise libzvbi uses
         * it to detect dropped frames. Unforunatey the guessed packet PTS values
         * (see mpegts demuxer) are not accurate enough to pass that test. */
        vbi_dvb_demux_cor(ctx->dx, ctx->sliced, 64, NULL, &pesheader_buf, &pesheader_size);

        ctx->handler_ret = pkt->size;

        while (left > 0) {
            int64_t pts = 0;
            unsigned int lines = vbi_dvb_demux_cor(ctx->dx, ctx->sliced, 64, &pts, &buf, &left);
#ifdef DEBUG
            av_log(avctx, AV_LOG_DEBUG,
                   "ctx=%p buf_size=%d left=%u lines=%u pts=%f pkt_pts=%f\n",
                   ctx, pkt->size, left, lines, (double)pts/90000.0, (double)pkt->pts/90000.0);
#endif
            if (lines > 0) {
#ifdef DEBUGx
                int i;
                for(i=0; i<lines; ++i)
                    av_log(avctx, AV_LOG_DEBUG,
                           "lines=%d id=%x\n", i, ctx->sliced[i].id);
#endif
                vbi_decode(ctx->vbi, ctx->sliced, lines, (double)pts/90000.0);
                ctx->lines_processed += lines;
            }
        }
        ctx->pts = AV_NOPTS_VALUE;
        ret = ctx->handler_ret;
    }

    if (ret < 0)
        return ret;

    // is there a subtitle to pass?
    if (ctx->nb_pages) {
        int i;
        sub->format = ctx->format_id;
        sub->start_display_time = 0;
        sub->end_display_time = ctx->sub_duration;
        sub->num_rects = 0;
        sub->pts = ctx->pages->pts;

        if (ctx->pages->sub_rect->type != SUBTITLE_NONE) {
            sub->rects = av_malloc(sizeof(*sub->rects) * 1);
            if (sub->rects) {
                sub->num_rects = 1;
                sub->rects[0] = ctx->pages->sub_rect;
            } else {
                ret = AVERROR(ENOMEM);
            }
        } else {
            av_log(avctx, AV_LOG_DEBUG, "sending empty sub\n");
            sub->rects = NULL;
        }
        if (!sub->rects) // no rect was passed
            subtitle_rect_free(&ctx->pages->sub_rect);

        for (i = 0; i < ctx->nb_pages - 1; i++)
            ctx->pages[i] = ctx->pages[i + 1];
        ctx->nb_pages--;

        if (ret >= 0)
            *data_size = 1;
    } else
        *data_size = 0;

    return ret;
}

static int teletext_init_decoder(AVCodecContext *avctx)
{
    TeletextContext *ctx = avctx->priv_data;
    unsigned int maj, min, rev;

    vbi_version(&maj, &min, &rev);
    if (!(maj > 0 || min > 2 || min == 2 && rev >= 26)) {
        av_log(avctx, AV_LOG_ERROR, "decoder needs zvbi version >= 0.2.26.\n");
        return AVERROR_EXTERNAL;
    }

    ctx->dx = NULL;
    ctx->vbi = NULL;
    ctx->pts = AV_NOPTS_VALUE;

#ifdef DEBUG
    {
        char *t;
        ctx->ex = vbi_export_new("text", &t);
    }
#endif
    av_log(avctx, AV_LOG_VERBOSE, "page filter: %s\n", ctx->pgno);
    return (ctx->format_id == 1) ? ff_ass_subtitle_header_default(avctx) : 0;
}

static int teletext_close_decoder(AVCodecContext *avctx)
{
    TeletextContext *ctx = avctx->priv_data;

#ifdef DEBUG
    av_log(avctx, AV_LOG_DEBUG, "lines_total=%u\n", ctx->lines_processed);
#endif
    while (ctx->nb_pages)
        subtitle_rect_free(&ctx->pages[--ctx->nb_pages].sub_rect);
    av_freep(&ctx->pages);

    vbi_dvb_demux_delete(ctx->dx);
    vbi_decoder_delete(ctx->vbi);
    ctx->dx = NULL;
    ctx->vbi = NULL;
    ctx->pts = AV_NOPTS_VALUE;
    return 0;
}

static void teletext_flush(AVCodecContext *avctx)
{
    teletext_close_decoder(avctx);
}

#define OFFSET(x) offsetof(TeletextContext, x)
#define SD AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    {"txt_page",        "list of teletext page numbers to decode, * is all", OFFSET(pgno),           AV_OPT_TYPE_STRING, {.str = "*"},      0, 0,        SD},
    {"txt_chop_top",    "discards the top teletext line",                    OFFSET(chop_top),       AV_OPT_TYPE_INT,    {.i64 = 1},        0, 1,        SD},
    {"txt_format",      "format of the subtitles (bitmap or text)",          OFFSET(format_id),      AV_OPT_TYPE_INT,    {.i64 = 0},        0, 1,        SD,  "txt_format"},
    {"bitmap",          NULL,                                                0,                      AV_OPT_TYPE_CONST,  {.i64 = 0},        0, 0,        SD,  "txt_format"},
    {"text",            NULL,                                                0,                      AV_OPT_TYPE_CONST,  {.i64 = 1},        0, 0,        SD,  "txt_format"},
    {"txt_left",        "x offset of generated bitmaps",                     OFFSET(x_offset),       AV_OPT_TYPE_INT,    {.i64 = 0},        0, 65535,    SD},
    {"txt_top",         "y offset of generated bitmaps",                     OFFSET(y_offset),       AV_OPT_TYPE_INT,    {.i64 = 0},        0, 65535,    SD},
    {"txt_chop_spaces", "chops leading and trailing spaces from text",       OFFSET(chop_spaces),    AV_OPT_TYPE_INT,    {.i64 = 1},        0, 1,        SD},
    {"txt_duration",    "display duration of teletext pages in msecs",       OFFSET(sub_duration),   AV_OPT_TYPE_INT,    {.i64 = 30000},    0, 86400000, SD},
    {"txt_transparent", "force transparent background of the teletext",      OFFSET(transparent_bg), AV_OPT_TYPE_INT,    {.i64 = 0},        0, 1,        SD},
    { NULL },
};

static const AVClass teletext_class = {
    .class_name = "libzvbi_teletextdec",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libzvbi_teletext_decoder = {
    .name      = "libzvbi_teletextdec",
    .long_name = NULL_IF_CONFIG_SMALL("Libzvbi DVB teletext decoder"),
    .type      = AVMEDIA_TYPE_SUBTITLE,
    .id        = AV_CODEC_ID_DVB_TELETEXT,
    .priv_data_size = sizeof(TeletextContext),
    .init      = teletext_init_decoder,
    .close     = teletext_close_decoder,
    .decode    = teletext_decode_frame,
    .capabilities = CODEC_CAP_DELAY,
    .flush     = teletext_flush,
    .priv_class= &teletext_class,
};
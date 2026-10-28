/* Native PDF content operations shared by CLI and future GUIs. */
#include "pt_pdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void append_stream(fz_context *ctx, pdf_document *doc, int page_num, const char *text)
{
    pdf_obj *page = pdf_lookup_page_obj(ctx, doc, page_num);
    pdf_obj *contents = pdf_dict_get(ctx, page, PDF_NAME(Contents));
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)text, strlen(text));
    pdf_obj *stream = NULL;
    fz_try(ctx) {
        stream = pdf_add_stream(ctx, doc, buf, NULL, 0);
        if (pdf_is_array(ctx, contents)) pdf_array_push(ctx, contents, stream);
        else {
            pdf_obj *arr = pdf_new_array(ctx, doc, 2);
            fz_try(ctx) {
                if (contents) pdf_array_push(ctx, arr, contents);
                pdf_array_push(ctx, arr, stream);
                pdf_dict_put(ctx, page, PDF_NAME(Contents), arr);
            }
            fz_always(ctx) pdf_drop_obj(ctx, arr);
            fz_catch(ctx) fz_rethrow(ctx);
        }
    }
    fz_always(ctx) {pdf_drop_obj(ctx, stream);fz_drop_buffer(ctx, buf);}
    fz_catch(ctx) fz_rethrow(ctx);
}

/* Always copy inherited resource dictionaries before changing them: changing a
 * shared inherited dictionary can otherwise affect unrelated pages. */
static pdf_obj *owned_resources(fz_context *ctx, pdf_document *doc, int page_num)
{
    pdf_obj *page = pdf_lookup_page_obj(ctx, doc, page_num);
    pdf_obj *old = pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources));
    pdf_obj *copy = old ? pdf_copy_dict(ctx, old) : pdf_new_dict(ctx, doc, 4);
    pdf_dict_put(ctx, page, PDF_NAME(Resources), copy);
    pdf_drop_obj(ctx, copy);
    return pdf_dict_get(ctx, page, PDF_NAME(Resources));
}

static fz_rect pdf_coords(fz_context *ctx, pdf_document *doc, int page_num, fz_rect box)
{
    pdf_page *page = pdf_load_page(ctx, doc, page_num);
    fz_matrix ctm;
    fz_try(ctx) pdf_page_transform(ctx, page, NULL, &ctm);
    fz_always(ctx) pdf_drop_page(ctx, page);
    fz_catch(ctx) fz_rethrow(ctx);
    return fz_transform_rect(box, fz_invert_matrix(ctm));
}

void pt_place_image(fz_context *ctx, pdf_document *doc, int page_num, const char *filename, fz_rect box)
{
    fz_image *img = fz_new_image_from_file(ctx, filename);
    pdf_obj *xref = NULL;
    fz_try(ctx) {
        fz_rect r = pdf_coords(ctx, doc, page_num, box);
        if (r.x1<=r.x0 || r.y1<=r.y0) fz_throw(ctx,FZ_ERROR_ARGUMENT,"empty image rectangle");
        xref = pdf_add_image(ctx, doc, img);
        char name[48]; snprintf(name,sizeof(name),"PTImage%d",pdf_to_num(ctx,xref));
        pdf_obj *resources = owned_resources(ctx, doc, page_num);
        pdf_obj *xobjs = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (xobjs) {
            pdf_obj *copy = pdf_copy_dict(ctx, xobjs);
            pdf_dict_put(ctx, resources, PDF_NAME(XObject), copy);
            pdf_drop_obj(ctx, copy);
        } else xobjs = pdf_dict_put_dict(ctx, resources, PDF_NAME(XObject), 4);
        xobjs=pdf_dict_get(ctx,resources,PDF_NAME(XObject));
        pdf_dict_puts(ctx, xobjs, name, xref);
        char cmd[256];
        snprintf(cmd,sizeof(cmd),"q\n%.5f 0 0 %.5f %.5f %.5f cm\n/%s Do\nQ\n",r.x1-r.x0,r.y1-r.y0,r.x0,r.y0,name);
        append_stream(ctx,doc,page_num,cmd);
    }
    fz_always(ctx) {pdf_drop_obj(ctx,xref);fz_drop_image(ctx,img);}
    fz_catch(ctx) fz_rethrow(ctx);
}

/* Actual PDF page-content text, not an annotation. Helvetica/WinAnsi only;
 * reject non-ASCII instead of silently corrupting Unicode. */
void pt_place_text(fz_context *ctx, pdf_document *doc, int page_num, const char *text,
                   float x, float y, float fontsize, const float rgb[3])
{
    if (fontsize<=0 || fontsize>200 || !isfinite(x)||!isfinite(y))
        fz_throw(ctx,FZ_ERROR_ARGUMENT,"invalid text position or size");
    size_t length=strlen(text);
    if(length>32000)fz_throw(ctx,FZ_ERROR_ARGUMENT,"text too long");
    for(size_t i=0;i<length;i++) if((unsigned char)text[i]>127 || text[i]=='\n' || text[i]=='\r')
        fz_throw(ctx,FZ_ERROR_ARGUMENT,"text insertion currently supports single-line ASCII only");
    fz_font *font=fz_new_base14_font(ctx,"Helvetica");
    pdf_obj *xref=NULL;
    fz_try(ctx){
        xref=pdf_add_simple_font(ctx,doc,font,0);
        char name[64];snprintf(name,sizeof(name),"PTFont%d",pdf_to_num(ctx,xref));
        pdf_obj *resources=owned_resources(ctx,doc,page_num);
        pdf_obj *fonts=pdf_dict_get(ctx,resources,PDF_NAME(Font));
        if(fonts){pdf_obj *copy=pdf_copy_dict(ctx,fonts);pdf_dict_put(ctx,resources,PDF_NAME(Font),copy);pdf_drop_obj(ctx,copy);}
        else pdf_dict_put_dict(ctx,resources,PDF_NAME(Font),4);
        pdf_dict_puts(ctx,pdf_dict_get(ctx,resources,PDF_NAME(Font)),name,xref);
        pdf_page *p=pdf_load_page(ctx,doc,page_num);
        fz_matrix ctm;
        pdf_page_transform(ctx,p,NULL,&ctm);
        fz_point pdfpt=fz_transform_point((fz_point){x,y},fz_invert_matrix(ctm));
        pdf_drop_page(ctx,p);
        char *cmd=malloc(length*4+512);
        if(!cmd)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
        size_t n=(size_t)snprintf(cmd,length*4+512,"q\n%.4f %.4f %.4f rg\nBT\n/%s %.3f Tf\n1 0 0 1 %.4f %.4f Tm\n(",rgb[0],rgb[1],rgb[2],name,fontsize,pdfpt.x,pdfpt.y);
        for(size_t i=0;i<length;i++){
            char c=text[i];
            if(c=='('||c==')'||c=='\\')cmd[n++]='\\';
            cmd[n++]=c;
        }
        const char *tail=") Tj\nET\nQ\n"; size_t tn=strlen(tail); memcpy(cmd+n,tail,tn+1); n+=tn;
        fz_try(ctx)append_stream(ctx,doc,page_num,cmd);
        fz_always(ctx)free(cmd);
        fz_catch(ctx)fz_rethrow(ctx);
    }
    fz_always(ctx){pdf_drop_obj(ctx,xref);fz_drop_font(ctx,font);}
    fz_catch(ctx)fz_rethrow(ctx);
}

void pt_append_image_page(fz_context *ctx, pdf_document *doc, const char *path)
{
    fz_image *image=fz_new_image_from_file(ctx,path);
    pdf_obj *ref=NULL, *resources=NULL,*page=NULL;
    fz_buffer *buf=NULL;
    fz_try(ctx){
        ref=pdf_add_image(ctx,doc,image);
        resources=pdf_new_dict(ctx,doc,2);
        pdf_obj *xobjs=pdf_dict_put_dict(ctx,resources,PDF_NAME(XObject),1);
        pdf_dict_puts(ctx,xobjs,"Im0",ref);
        const char *draw="q 0 0 1 1 re W n 1 0 0 1 0 0 cm /Im0 Do Q\n";
        /* PDF image XObject paints into a unit square, scaled to the page size. */
        float w=(float)image->w,h=(float)image->h;
        if(w<=0||h<=0)fz_throw(ctx,FZ_ERROR_ARGUMENT,"invalid image dimensions");
        char command[160];snprintf(command,sizeof(command),"q %.3f 0 0 %.3f 0 0 cm /Im0 Do Q\n",w,h);
        (void)draw;
        buf=fz_new_buffer_from_copied_data(ctx,(unsigned char*)command,strlen(command));
        page=pdf_add_page(ctx,doc,(fz_rect){0,0,w,h},0,resources,buf);
        pdf_insert_page(ctx,doc,-1,page);
    }
    fz_always(ctx){pdf_drop_obj(ctx,page);pdf_drop_obj(ctx,resources);pdf_drop_obj(ctx,ref);fz_drop_buffer(ctx,buf);fz_drop_image(ctx,image);}
    fz_catch(ctx)fz_rethrow(ctx);
}

pdf_document *pt_flatten(fz_context *ctx, pdf_document *source, int dpi)
{
    if(dpi<36||dpi>600)fz_throw(ctx,FZ_ERROR_ARGUMENT,"flatten dpi must be 36..600");
    pdf_document *out=pdf_create_document(ctx);
    fz_pixmap *pix=NULL;
    fz_image *img=NULL;
    pdf_obj *resources=NULL,*xref=NULL,*pg=NULL;
    fz_buffer *buf=NULL;
    fz_try(ctx){
        int n=pdf_count_pages(ctx,source);
        for(int i=0;i<n;i++){
            pix=pt_render(ctx,source,i,(float)dpi/72);
            img=fz_new_image_from_pixmap(ctx,pix,NULL);
            xref=pdf_add_image(ctx,out,img);
            resources=pdf_new_dict(ctx,out,1);
            pdf_dict_puts(ctx,pdf_dict_put_dict(ctx,resources,PDF_NAME(XObject),1),"Im0",xref);
            pdf_page *original=pdf_load_page(ctx,source,i);
            fz_rect rect=fz_bound_page(ctx,(fz_page*)original);
            pdf_drop_page(ctx,original);
            float w=rect.x1-rect.x0,h=rect.y1-rect.y0;
            char cmd[128];snprintf(cmd,sizeof(cmd),"q %.3f 0 0 %.3f 0 0 cm /Im0 Do Q\n",w,h);
            buf=fz_new_buffer_from_copied_data(ctx,(unsigned char*)cmd,strlen(cmd));
            pg=pdf_add_page(ctx,out,(fz_rect){0,0,w,h},0,resources,buf);
            pdf_insert_page(ctx,out,-1,pg);
            pdf_drop_obj(ctx,pg);pg=NULL;pdf_drop_obj(ctx,resources);resources=NULL;
            pdf_drop_obj(ctx,xref);xref=NULL;fz_drop_buffer(ctx,buf);buf=NULL;
            fz_drop_image(ctx,img);img=NULL;fz_drop_pixmap(ctx,pix);pix=NULL;
        }
    }
    fz_always(ctx){pdf_drop_obj(ctx,pg);pdf_drop_obj(ctx,resources);pdf_drop_obj(ctx,xref);fz_drop_buffer(ctx,buf);fz_drop_image(ctx,img);fz_drop_pixmap(ctx,pix);}
    fz_catch(ctx){pdf_drop_document(ctx,out);fz_rethrow(ctx);}
    return out;
}

int pt_redact_text(fz_context *ctx, pdf_document *doc, const char *needle, int only_page)
{
    if(!needle||!*needle)fz_throw(ctx,FZ_ERROR_ARGUMENT,"empty redaction search text");
    int hit_count=0;
    int first=only_page<0?0:only_page, end=only_page<0?pdf_count_pages(ctx,doc):only_page+1;
    for(int i=first;i<end;i++){
        pdf_page *pg=pdf_load_page(ctx,doc,i);
        fz_try(ctx){
            int marks[2048];fz_quad quads[2048];
            int matches=fz_search_page(ctx,(fz_page*)pg,needle,marks,quads,2048);
            for(int j=0;j<matches;j++){
                fz_rect r=fz_rect_from_quad(quads[j]);
                pdf_annot *annot=pdf_create_annot(ctx,pg,PDF_ANNOT_REDACT);
                fz_try(ctx){
                    pdf_set_annot_rect(ctx,annot,r);
                    pdf_apply_redaction(ctx,annot,NULL);
                    hit_count++;
                }
                fz_always(ctx)pdf_drop_annot(ctx,annot);
                fz_catch(ctx)fz_rethrow(ctx);
            }
            if(matches==2048)fz_throw(ctx,FZ_ERROR_LIMIT,"too many matches on one page: refusing incomplete redaction");
        }
        fz_always(ctx)pdf_drop_page(ctx,pg);
        fz_catch(ctx)fz_rethrow(ctx);
    }
    return hit_count;
}

/* Remove common non-page-data sources. This does NOT provide a formal forensic guarantee. */
void pt_scrub(fz_context *ctx, pdf_document *doc)
{
    pdf_obj *root=pdf_dict_get(ctx,pdf_trailer(ctx,doc),PDF_NAME(Root));
    pdf_obj *names=pdf_dict_get(ctx,root,PDF_NAME(Names));
    if(names){pdf_dict_dels(ctx,names,"EmbeddedFiles");pdf_dict_dels(ctx,names,"JavaScript");}
    pdf_dict_dels(ctx,root,"Metadata");
    pdf_dict_dels(ctx,root,"OpenAction");
    pdf_dict_dels(ctx,root,"AA");
    pdf_dict_dels(ctx,pdf_trailer(ctx,doc),"Info");
    for(int i=0;i<pdf_count_pages(ctx,doc);i++){
        pdf_obj *page=pdf_lookup_page_obj(ctx,doc,i);
        pdf_dict_dels(ctx,page,"AA");
        pdf_dict_dels(ctx,page,"Thumb");
        pdf_dict_dels(ctx,page,"Metadata");
    }
}

/* Annotation indexes are zero based and refer to ordinary annotations, not form widgets. */
int pt_modify_annot(fz_context *ctx,pdf_document *doc,int page,int index,int remove,const char *comment)
{
    pdf_page *pg=pdf_load_page(ctx,doc,page);
    int found=0,j=0;
    fz_try(ctx){
        for(pdf_annot *a=pdf_first_annot(ctx,pg);a;a=pdf_next_annot(ctx,a)){
            if(j++==index){
                if(remove)pdf_delete_annot(ctx,pg,a);
                else {pdf_set_annot_contents(ctx,a,comment?comment:"");pdf_update_annot(ctx,a);}
                found=1;break;
            }
        }
    }
    fz_always(ctx)pdf_drop_page(ctx,pg);
    fz_catch(ctx)fz_rethrow(ctx);
    return found;
}

/* Compute visible content bounds, excluding annotation decorations, and crop to
 * them. This changes the CropBox; it does not securely delete outside content. */
void pt_crop_content(fz_context *ctx,pdf_document *doc,int page_no,float margin)
{
    pdf_page *page=pdf_load_page(ctx,doc,page_no);
    fz_device *device=NULL;
    fz_rect r=fz_empty_rect;
    fz_try(ctx){
        device=fz_new_bbox_device(ctx,&r);
        pdf_run_page_contents(ctx,page,device,fz_identity,NULL);
        fz_close_device(ctx,device);
        fz_rect bounds=fz_bound_page(ctx,(fz_page*)page);
        if(!fz_is_empty_rect(r) && !fz_is_infinite_rect(r)){
            r.x0=fmaxf(bounds.x0,r.x0-margin);r.y0=fmaxf(bounds.y0,r.y0-margin);
            r.x1=fminf(bounds.x1,r.x1+margin);r.y1=fminf(bounds.y1,r.y1+margin);
            if(r.x0<r.x1 && r.y0<r.y1)pdf_set_page_box(ctx,page,FZ_CROP_BOX,r);
        }
    }
    fz_always(ctx){fz_drop_device(ctx,device);pdf_drop_page(ctx,page);}
    fz_catch(ctx)fz_rethrow(ctx);
}

#ifdef PT_USE_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
typedef struct { fz_rect *rect; size_t length, capacity; } pt_rect_list;
static void pt_push_rect(fz_context *ctx,pt_rect_list *a,fz_rect r)
{
    if(a->length==a->capacity){
        size_t cap=a->capacity?2*a->capacity:32;
        fz_rect *p=realloc(a->rect,cap*sizeof(*p));
        if(!p)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
        a->rect=p;a->capacity=cap;
    }
    a->rect[a->length++]=r;
}

int pt_redact_regex(fz_context *ctx,pdf_document *doc,const char *pattern,int only_page,int sensitive)
{
    int err;PCRE2_SIZE off=0;
    uint32_t flags=PCRE2_UTF | (sensitive?0:PCRE2_CASELESS);
    pcre2_code *re=pcre2_compile((PCRE2_SPTR)pattern,PCRE2_ZERO_TERMINATED,flags,&err,&off,NULL);
    if(!re)fz_throw(ctx,FZ_ERROR_ARGUMENT,"invalid regex at byte %llu (PCRE2 code %d)",(unsigned long long)off,err);
    pcre2_match_data *matches=pcre2_match_data_create_from_pattern(re,NULL);
    int total=0;
    fz_try(ctx){
        int first=only_page<0?0:only_page, end=only_page<0?pdf_count_pages(ctx,doc):only_page+1;
        for(int page_no=first;page_no<end;page_no++){
            pdf_page *page=pdf_load_page(ctx,doc,page_no);
            fz_stext_page *st=NULL;
            char *str=NULL;
            fz_rect *boxes=NULL;
            int *lines=NULL;
            pt_rect_list affected={0};
            fz_try(ctx){
                st=fz_new_stext_page_from_page(ctx,(fz_page *)page,NULL);
                size_t cap=4096,n=0;
                str=malloc(cap);boxes=malloc(cap*sizeof(*boxes));lines=malloc(cap*sizeof(*lines));
                if(!str||!boxes||!lines)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
                int line_id=0;
                for(fz_stext_block *b=st->first_block;b;b=b->next){
                    if(b->type!=FZ_STEXT_BLOCK_TEXT)continue;
                    for(fz_stext_line *l=b->u.t.first_line;l;l=l->next){
                        for(fz_stext_char *c=l->first_char;c;c=c->next){
                            char bytes[8];int byte_count=fz_runetochar(bytes,c->c);
                            if(byte_count<1||byte_count>4)continue;
                            while(n+(size_t)byte_count+2>=cap){
                                cap*=2;
                                if(cap>2*1024*1024)fz_throw(ctx,FZ_ERROR_LIMIT,"page text exceeds regex limit");
                                char *ss=realloc(str,cap);
                                if(!ss)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
                                str=ss;
                                fz_rect *rr=realloc(boxes,cap*sizeof(*rr));
                                if(!rr)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
                                boxes=rr;
                                int *ll=realloc(lines,cap*sizeof(*ll));
                                if(!ll)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
                                lines=ll;
                            }
                            for(int k=0;k<byte_count;k++){
                                str[n]=bytes[k];boxes[n]=fz_rect_from_quad(c->quad);lines[n]=line_id;n++;
                            }
                        }
                        if(n+2>=cap)fz_throw(ctx,FZ_ERROR_LIMIT,"page too large");
                        str[n]='\n';boxes[n]=fz_empty_rect;lines[n]=-1;n++;line_id++;
                    }
                }
                str[n]=0;
                PCRE2_SIZE start=0;
                while(start<n){
                    int rc=pcre2_match(re,(PCRE2_SPTR)str,n,start,0,matches,NULL);
                    if(rc==PCRE2_ERROR_NOMATCH)break;
                    if(rc<0)fz_throw(ctx,FZ_ERROR_ARGUMENT,"regex match error %d",rc);
                    PCRE2_SIZE *ovec=pcre2_get_ovector_pointer(matches);
                    size_t a=(size_t)ovec[0],z=(size_t)ovec[1];
                    if(a>=z){start=z+1;continue;}
                    int last_line=-1;fz_rect box=fz_empty_rect;
                    for(size_t j=a;j<z;j++)if(lines[j]>=0){
                        if(last_line!=-1 && lines[j]!=last_line){
                            pt_push_rect(ctx,&affected,box);box=fz_empty_rect;
                        }
                        box=fz_union_rect(box,boxes[j]);last_line=lines[j];
                    }
                    if(last_line>=0){pt_push_rect(ctx,&affected,box);total++;}
                    start=z;
                }
                for(size_t j=0;j<affected.length;j++){
                    pdf_annot *annot=pdf_create_annot(ctx,page,PDF_ANNOT_REDACT);
                    fz_try(ctx){pdf_set_annot_rect(ctx,annot,affected.rect[j]);pdf_apply_redaction(ctx,annot,NULL);}
                    fz_always(ctx)pdf_drop_annot(ctx,annot);
                    fz_catch(ctx)fz_rethrow(ctx);
                }
            }
            fz_always(ctx){free(affected.rect);free(str);free(boxes);free(lines);fz_drop_stext_page(ctx,st);pdf_drop_page(ctx,page);}
            fz_catch(ctx)fz_rethrow(ctx);
        }
    }
    fz_always(ctx){pcre2_match_data_free(matches);pcre2_code_free(re);}
    fz_catch(ctx)fz_rethrow(ctx);
    return total;
}
#endif

/* Copy form widget hierarchies as well as page objects. pdf_graft_page alone
 * transfers /Annots but does not register those widgets under /AcroForm. */
void pt_graft_pages_with_forms(fz_context *ctx,pdf_document *dst,pdf_document *src,
                              const int *pages,int count)
{
    pdf_graft_map *map=pdf_new_graft_map(ctx,dst);
    fz_try(ctx){
        pdf_obj *srcroot=pdf_dict_get(ctx,pdf_trailer(ctx,src),PDF_NAME(Root));
        pdf_obj *srcform=pdf_dict_get(ctx,srcroot,PDF_NAME(AcroForm));
        pdf_obj *dstroot=pdf_dict_get(ctx,pdf_trailer(ctx,dst),PDF_NAME(Root));
        pdf_obj *dstform=NULL,*fields=NULL;
        for(int j=0;j<count;j++){
            int id=pages[j];
            pdf_graft_mapped_page(ctx,map,-1,src,id);
            /* MuPDF page grafting omits /Annots by design: restore them via
             * the same object map before registering AcroForm roots. */
            pdf_obj *original=pdf_lookup_page_obj(ctx,src,id);
            pdf_obj *source_annots=pdf_dict_get(ctx,original,PDF_NAME(Annots));
            if(source_annots){
                pdf_obj *copy_annots=pdf_graft_mapped_object(ctx,map,source_annots);
                pdf_obj *newpage=pdf_lookup_page_obj(ctx,dst,pdf_count_pages(ctx,dst)-1);
                pdf_dict_put(ctx,newpage,PDF_NAME(Annots),copy_annots);
                pdf_drop_obj(ctx,copy_annots);
            }
            if(!srcform)continue;
            pdf_page *pg=pdf_load_page(ctx,src,id);
            fz_try(ctx){
                for(pdf_annot *w=pdf_first_widget(ctx,pg);w;w=pdf_next_widget(ctx,w)){
                    pdf_obj *field=pdf_annot_obj(ctx,w);
                    pdf_obj *parent;
                    while((parent=pdf_dict_get(ctx,field,PDF_NAME(Parent))))field=parent;
                    pdf_obj *copy=pdf_graft_mapped_object(ctx,map,field);
                    fz_try(ctx){
                        if(!dstform){
                            dstform=pdf_dict_get(ctx,dstroot,PDF_NAME(AcroForm));
                            if(!dstform){
                                pdf_obj *d=pdf_new_dict(ctx,dst,3);
                                pdf_dict_put(ctx,dstroot,PDF_NAME(AcroForm),d);pdf_drop_obj(ctx,d);
                                dstform=pdf_dict_get(ctx,dstroot,PDF_NAME(AcroForm));
                            }
                            fields=pdf_dict_get(ctx,dstform,PDF_NAME(Fields));
                            if(!fields){
                                pdf_obj *a=pdf_new_array(ctx,dst,4);
                                pdf_dict_put(ctx,dstform,PDF_NAME(Fields),a);pdf_drop_obj(ctx,a);
                                fields=pdf_dict_get(ctx,dstform,PDF_NAME(Fields));
                            }
                            pdf_obj *dr=pdf_dict_get(ctx,srcform,PDF_NAME(DR));
                            if(dr && !pdf_dict_get(ctx,dstform,PDF_NAME(DR))){
                                pdf_obj *clone=pdf_graft_mapped_object(ctx,map,dr);
                                pdf_dict_put(ctx,dstform,PDF_NAME(DR),clone);pdf_drop_obj(ctx,clone);
                            }
                        }
                        if(!pdf_array_contains(ctx,fields,copy))pdf_array_push(ctx,fields,copy);
                    }
                    fz_always(ctx)pdf_drop_obj(ctx,copy);
                    fz_catch(ctx)fz_rethrow(ctx);
                }
            }
            fz_always(ctx)pdf_drop_page(ctx,pg);
            fz_catch(ctx)fz_rethrow(ctx);
        }
    }
    fz_always(ctx)pdf_drop_graft_map(ctx,map);
    fz_catch(ctx)fz_rethrow(ctx);
}

/* Turn a white-paper scanned signature into a tightly-cropped transparent PNG.
 * Pixels above the white threshold become transparent; foreground is black. */
void pt_prepare_signature(fz_context *ctx,const char *input,const char *output)
{
    fz_image *img=fz_new_image_from_file(ctx,input);
    fz_pixmap *source=NULL,*rgb=NULL,*result=NULL;
    fz_try(ctx){
        source=fz_get_pixmap_from_image(ctx,img,NULL,NULL,NULL,NULL);
        rgb=fz_convert_pixmap(ctx,source,fz_device_rgb(ctx),NULL,NULL,fz_default_color_params,0);
        int w=rgb->w,h=rgb->h,minx=w,miny=h,maxx=-1,maxy=-1;
        if(w>10000||h>10000)fz_throw(ctx,FZ_ERROR_LIMIT,"signature image too large");
        for(int y=0;y<h;y++)for(int x=0;x<w;x++){
            const unsigned char *p=rgb->samples+y*rgb->stride+x*3;
            int gray=(p[0]*299+p[1]*587+p[2]*114)/1000;
            if(gray<225){
                if(x<minx)minx=x;if(x>maxx)maxx=x;
                if(y<miny)miny=y;if(y>maxy)maxy=y;
            }
        }
        if(maxx<minx||maxy<miny)fz_throw(ctx,FZ_ERROR_ARGUMENT,"no signature ink found");
        int ow=maxx-minx+1,oh=maxy-miny+1;
        result=fz_new_pixmap(ctx,fz_device_rgb(ctx),ow,oh,NULL,1);
        for(int y=0;y<oh;y++)for(int x=0;x<ow;x++){
            const unsigned char *in=rgb->samples+(miny+y)*rgb->stride+(minx+x)*3;
            unsigned char *out=result->samples+y*result->stride+x*4;
            int gray=(in[0]*299+in[1]*587+in[2]*114)/1000;
            int a=(225-gray)*2;
            if(a<0)a=0;if(a>255)a=255;
            out[0]=out[1]=out[2]=0;out[3]=(unsigned char)a;
        }
        fz_save_pixmap_as_png(ctx,result,output);
    }
    fz_always(ctx){fz_drop_pixmap(ctx,result);fz_drop_pixmap(ctx,rgb);fz_drop_pixmap(ctx,source);fz_drop_image(ctx,img);}
    fz_catch(ctx)fz_rethrow(ctx);
}

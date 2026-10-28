#include "pt_pdf.h"
#include "pt_ranges.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static void help(void) {
    puts("pdf-tools-c: native MuPDF PDF operations (pages are 1-based)\n"
         "  info INPUT [--password P]\n"
         "  merge -o OUT INPUT1.pdf INPUT2.pdf ...\n"
         "  split INPUT (--every N | --at 3,7) -d DIRECTORY\n"
         "  pages INPUT -o OUT [--order 3,1-2] [--remove 2,4] [--rotate PAGE:DEGREES] [--crop PAGE:X0,Y0,X1,Y1]\n"
         "  annotate INPUT -o OUT --page N --type highlight|underline|strike|squiggly|note|text|rect|line\n"
         "           --rect X0,Y0,X1,Y1 [--text STRING] [--color R,G,B]\n"
         "  redact INPUT -o OUT --page N --rect X0,Y0,X1,Y1\n"
         "  draw INPUT -o OUT --page N --points X,Y;X,Y;... [--width POINTS]\n"
         "  search INPUT --text STRING\n"
         "  fill INPUT -o OUT --field NAME=VALUE (repeat --field)\n"
         "  render INPUT -o OUT.png [--page N] [--dpi 144]\n"
         "  protect INPUT -o OUT --new-password PASSWORD\n"
         "  unprotect INPUT -o OUT --password PASSWORD\n"
         "All modifying commands write a new file. Password input can be provided via --password.\n"
         "Coordinates are PDF points, top-left origin (1/72 inch)." );
}
static const char *opt(int argc, char **argv, const char *name) {
    for (int i=2; i+1<argc; ++i) if (!strcmp(argv[i],name)) return argv[i+1];
    return NULL;
}
static const char *required(const char *value, const char *description, fz_context *ctx) {
    if (!value) fz_throw(ctx, FZ_ERROR_ARGUMENT, "missing %s", description);
    return value;
}
static const char *out_path(int argc,char **argv, const char *input,fz_context *ctx) {
    const char *out = required(opt(argc,argv,"-o"),"-o OUT",ctx);
    if (input && !strcmp(input,out)) fz_throw(ctx,FZ_ERROR_ARGUMENT,"output must differ from input");
    return out;
}
static int number(const char *s,int fallback) { return s ? atoi(s) : fallback; }
static int page_no(int argc, char **argv, fz_context *ctx, pdf_document *doc) {
    int n=number(opt(argc,argv,"--page"),1);
    if (n<1 || n>pdf_count_pages(ctx,doc)) fz_throw(ctx,FZ_ERROR_ARGUMENT,"page out of bounds");
    return n-1;
}
static fz_rect parse_rect(const char *v, fz_context *ctx) {
    fz_rect r;
    if (!v || sscanf(v,"%f,%f,%f,%f",&r.x0,&r.y0,&r.x1,&r.y1)!=4 ||
        !isfinite(r.x0) || !isfinite(r.y0) || !isfinite(r.x1) || !isfinite(r.y1))
        fz_throw(ctx,FZ_ERROR_ARGUMENT,"expected --rect x0,y0,x1,y1");
    return r;
}
static void color_of(const char *v, float rgb[3]) {
    rgb[0]=1; rgb[1]=0.8f; rgb[2]=0.18f;
    if (v) { float a,b,c; if (sscanf(v,"%f,%f,%f",&a,&b,&c)==3) {
        rgb[0]=fmaxf(0,fminf(1,a));rgb[1]=fmaxf(0,fminf(1,b));rgb[2]=fmaxf(0,fminf(1,c));
    }}
}
static void save_new(fz_context *ctx,pdf_document *doc,const char *out,const char *optstr) {
    pt_save(ctx,doc,out,optstr);
    printf("{\"output\":\"%s\",\"ok\":true}\n",out);
}
static void cmd_info(fz_context *ctx,pdf_document *doc) {
    int n=pdf_count_pages(ctx,doc);
    printf("{\"pages\":%d,\"encrypted\":%s,\"sizes\":[", n,
           pdf_needs_password(ctx,doc)?"true":"false");
    for(int i=0;i<n;i++){
        pdf_page *p=pdf_load_page(ctx,doc,i);
        fz_rect r=fz_bound_page(ctx,(fz_page*)p);
        printf("%s[%.2f,%.2f]",i?",":"",r.x1-r.x0,r.y1-r.y0);
        pdf_drop_page(ctx,p);
    }
    puts("]}");
}
static void cmd_merge(fz_context *ctx,int argc,char **argv) {
    const char *out=out_path(argc,argv,NULL,ctx);
    pdf_document *dst=pdf_create_document(ctx);
    int count=0;
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"-o")){i++;continue;}
        pdf_document *src=pt_open(ctx,argv[i],NULL);
        int n=pdf_count_pages(ctx,src);
        for(int j=0;j<n;j++){pdf_graft_page(ctx,dst,-1,src,j);count++;}
        pdf_drop_document(ctx,src);
    }
    if(!count)fz_throw(ctx,FZ_ERROR_ARGUMENT,"merge needs at least one input PDF");
    save_new(ctx,dst,out,NULL);
    pdf_drop_document(ctx,dst);
}
static void cmd_split(fz_context *ctx, int argc,char **argv,pdf_document *src) {
    int every=number(opt(argc,argv,"--every"),0);
    const char *at=opt(argc,argv,"--at");
    const char *dir=required(opt(argc,argv,"-d"),"-d DIRECTORY",ctx);
    if ((!at && every<1) || (at && every))
        fz_throw(ctx,FZ_ERROR_ARGUMENT,"use exactly one of --every N or --at PAGES");
    if(MKDIR(dir)!=0 && errno!=EEXIST)fz_throw(ctx,FZ_ERROR_ARGUMENT,"cannot create output directory");
    int n=pdf_count_pages(ctx,src), part=0;
    int *starts=calloc((size_t)n+1,sizeof(int));
    if(!starts)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
    starts[0]=0;
    if(at) {
        int *parsed=calloc((size_t)n+1,sizeof(int));
        int cnt=pt_parse_pages(at,n,parsed,(size_t)n+1);
        if(cnt<0)fz_throw(ctx,FZ_ERROR_ARGUMENT,"invalid --at page list");
        for(int i=0;i<cnt;i++) starts[parsed[i]]=1;
        free(parsed);
    } else for(int i=0;i<n;i+=every)starts[i]=1;
    starts[n]=1;
    for(int a=0;a<n;) {
        int b=a+1;
        while(b<n&&!starts[b])b++;
        int len=b-a;
        int *pages=malloc((size_t)len*sizeof(int));
        if(!pages)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
        for(int j=0;j<len;j++)pages[j]=a+j;
        pdf_document *dst=pt_copy_pages(ctx,src,pages,len);
        free(pages);
        char name[1024];snprintf(name,sizeof(name),"%s/part-%03d.pdf",dir,++part);
        pt_save(ctx,dst,name,NULL);
        pdf_drop_document(ctx,dst);
        a=b;
    }
    free(starts);
    printf("{\"parts\":%d,\"directory\":\"%s\"}\n",part,dir);
}
static void cmd_pages(fz_context *ctx,int argc,char **argv,pdf_document *src) {
    int n=pdf_count_pages(ctx,src);
    const char *spec=opt(argc,argv,"--order");
    int *order=malloc((size_t)(n+8192)*sizeof(int));
    if(!order)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
    int count=spec?pt_parse_pages(spec,n,order,(size_t)n+8192):n;
    if(!spec)for(int j=0;j<n;j++)order[j]=j;
    if(count<1)fz_throw(ctx,FZ_ERROR_ARGUMENT,"invalid page range");
    const char *remove=opt(argc,argv,"--remove");
    if(remove) {
        int *remove_pages=calloc((size_t)n,sizeof(int));
        unsigned char *drop=calloc((size_t)n,1);
        if(!remove_pages||!drop)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
        int nr=pt_parse_pages(remove,n,remove_pages,n);
        if(nr<0)fz_throw(ctx,FZ_ERROR_ARGUMENT,"invalid --remove page range");
        for(int j=0;j<nr;j++)drop[remove_pages[j]]=1;
        int w=0;
        for(int j=0;j<count;j++)if(!drop[order[j]])order[w++]=order[j];
        count=w;
        free(remove_pages);free(drop);
        if(count<1)fz_throw(ctx,FZ_ERROR_ARGUMENT,"cannot remove every page");
    }
    pdf_document *dst=pt_copy_pages(ctx,src,order,count);
    free(order);
    const char *rot=opt(argc,argv,"--rotate");
    if(rot) {
        int p=0,d=0;
        if(sscanf(rot,"%d:%d",&p,&d)!=2 || p<1 || p>count)
            fz_throw(ctx,FZ_ERROR_ARGUMENT,"expected --rotate PAGE:DEGREES");
        pt_rotate(ctx,dst,p-1,d);
    }
    const char *crop=opt(argc,argv,"--crop");
    if(crop){
        int p=0;float a,b,c,d;
        if(sscanf(crop,"%d:%f,%f,%f,%f",&p,&a,&b,&c,&d)!=5||p<1||p>count)
            fz_throw(ctx,FZ_ERROR_ARGUMENT,"expected --crop PAGE:X0,Y0,X1,Y1");
        pdf_page *pg=pdf_load_page(ctx,dst,p-1);
        fz_rect box={a,b,c,d};
        pdf_set_page_box(ctx,pg,FZ_CROP_BOX,box);
        pdf_drop_page(ctx,pg);
    }
    save_new(ctx,dst,out_path(argc,argv,argv[2],ctx),NULL);
    pdf_drop_document(ctx,dst);
}
static pt_tool parse_type(fz_context *ctx,const char *name) {
    if(!name)fz_throw(ctx,FZ_ERROR_ARGUMENT,"missing --type");
    struct {const char *name;pt_tool val;} types[]={
        {"highlight",PT_HIGHLIGHT},{"underline",PT_UNDERLINE},{"strike",PT_STRIKE},
        {"squiggly",PT_SQUIGGLY},{"note",PT_NOTE},{"text",PT_TEXT},
        {"rect",PT_RECT},{"line",PT_LINE}
    };
    for(size_t j=0;j<sizeof(types)/sizeof(types[0]);j++)if(!strcmp(name,types[j].name))return types[j].val;
    fz_throw(ctx,FZ_ERROR_ARGUMENT,"unknown annotation type");
    return PT_NOTE;
}
static void cmd_annotate(fz_context *ctx,int argc,char **argv,pdf_document *doc,int redact) {
    float rgb[3];color_of(opt(argc,argv,"--color"),rgb);
    if(redact){rgb[0]=rgb[1]=rgb[2]=0;}
    int page=page_no(argc,argv,ctx,doc);
    pt_tool tool=redact?PT_REDACT:parse_type(ctx,opt(argc,argv,"--type"));
    pt_add_rect(ctx,doc,page,tool,parse_rect(opt(argc,argv,"--rect"),ctx),
        opt(argc,argv,"--text"),rgb);
    save_new(ctx,doc,out_path(argc,argv,argv[2],ctx),NULL);
}
static void cmd_draw(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    const char *str=required(opt(argc,argv,"--points"),"--points",ctx);
    size_t max=strlen(str)/3+2;
    fz_point *points=calloc(max,sizeof(fz_point));
    if(!points)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
    int n=0;
    const char *p=str;
    while(*p){
        float x,y;int chars=0;
        if(sscanf(p," %f , %f %n",&x,&y,&chars)!=2 || chars==0 || n>=(int)max)
            fz_throw(ctx,FZ_ERROR_ARGUMENT,"points must be x,y;x,y;...");
        points[n++]=(fz_point){x,y};p+=chars;
        if(!*p)break;
        if(*p++!=';')fz_throw(ctx,FZ_ERROR_ARGUMENT,"points require semicolon separators");
    }
    float rgb[3];color_of(opt(argc,argv,"--color"),rgb);
    pt_add_stroke(ctx,doc,page_no(argc,argv,ctx,doc),points,n,
                  (float)atof(opt(argc,argv,"--width")?opt(argc,argv,"--width"):"2"),rgb);
    free(points);
    save_new(ctx,doc,out_path(argc,argv,argv[2],ctx),NULL);
}
static void cmd_fill(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    int hits=0;
    for(int a=3;a+1<argc;a++)if(!strcmp(argv[a],"--field")){
        const char *arg=argv[++a];
        const char *sep=strchr(arg,'=');
        if(!sep || sep==arg)fz_throw(ctx,FZ_ERROR_ARGUMENT,"--field must be NAME=VALUE");
        size_t n=(size_t)(sep-arg);
        char *name=malloc(n+1);
        if(!name)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
        memcpy(name,arg,n);name[n]=0;
        int matched=0;
        for(int i=0;i<pdf_count_pages(ctx,doc);i++){
            pdf_page *page=pdf_load_page(ctx,doc,i);
            for(pdf_annot *widget=pdf_first_widget(ctx,page);widget;widget=pdf_next_widget(ctx,widget)){
                pdf_obj *field=pdf_annot_obj(ctx,widget);
                char *wname=pdf_load_field_name(ctx,field);
                if(wname && !strcmp(name,wname)){
                    if(pdf_set_field_value(ctx,doc,field,sep+1,1)) {
                        pdf_update_widget(ctx,widget);matched=1;hits++;
                    }
                }
                fz_free(ctx,wname);
            }
            pdf_drop_page(ctx,page);
        }
        free(name);
        if(!matched)fz_throw(ctx,FZ_ERROR_ARGUMENT,"no such field or invalid value: %s",arg);
    }
    if(!hits)fz_throw(ctx,FZ_ERROR_ARGUMENT,"fill needs --field NAME=VALUE");
    save_new(ctx,doc,out_path(argc,argv,argv[2],ctx),NULL);
}
static void cmd_search(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    const char *needle=required(opt(argc,argv,"--text"),"--text",ctx);
    int n=pdf_count_pages(ctx,doc), hits=0;
    printf("{\"matches\":[");
    for(int i=0;i<n;i++){
        pdf_page *p=pdf_load_page(ctx,doc,i);
        int marks[256];fz_quad q[256];
        int found=fz_search_page(ctx,(fz_page*)p,needle,marks,q,256);
        for(int j=0;j<found;j++){
            fz_rect b=fz_rect_from_quad(q[j]);
            printf("%s{\"page\":%d,\"rect\":[%.2f,%.2f,%.2f,%.2f]}",
                hits++?",":"",i+1,b.x0,b.y0,b.x1,b.y1);
        }
        pdf_drop_page(ctx,p);
    }
    printf("],\"count\":%d}\n",hits);
}
static void cmd_render(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    int dpi=number(opt(argc,argv,"--dpi"),144);
    if(dpi<36 || dpi>600)fz_throw(ctx,FZ_ERROR_ARGUMENT,"dpi must be 36..600");
    fz_pixmap *pix=pt_render(ctx,doc,page_no(argc,argv,ctx,doc),(float)dpi/72);
    const char *out=out_path(argc,argv,argv[2],ctx);
    fz_save_pixmap_as_png(ctx,pix,out);
    fz_drop_pixmap(ctx,pix);
    printf("{\"output\":\"%s\",\"ok\":true}\n",out);
}
static void cmd_password(fz_context *ctx,int argc,char **argv,pdf_document *doc,int protect) {
    const char *out=out_path(argc,argv,argv[2],ctx);
    if(!protect){save_new(ctx,doc,out,"encrypt=none,garbage=compact,compress");return;}
    const char *pwd=required(opt(argc,argv,"--new-password"),"--new-password",ctx);
    if(strchr(pwd,',') || strlen(pwd)>120)
        fz_throw(ctx,FZ_ERROR_ARGUMENT,"password cannot contain comma or exceed 120 bytes in this CLI");
    char opts[320];snprintf(opts,sizeof(opts),"encrypt=aes-256,user-password=%s,owner-password=%s,garbage=compact,compress",pwd,pwd);
    save_new(ctx,doc,out,opts);
}
static void dispatch(fz_context *ctx,int argc,char **argv) {
    const char *cmd=argv[1];
    if(!strcmp(cmd,"merge")){cmd_merge(ctx,argc,argv);return;}
    if(argc<3)fz_throw(ctx,FZ_ERROR_ARGUMENT,"missing INPUT");
    const char *input=argv[2];
    pdf_document *doc=pt_open(ctx,input,opt(argc,argv,"--password"));
    fz_try(ctx) {
        if(!strcmp(cmd,"info"))cmd_info(ctx,doc);
        else if(!strcmp(cmd,"split"))cmd_split(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"pages"))cmd_pages(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"annotate"))cmd_annotate(ctx,argc,argv,doc,0);
        else if(!strcmp(cmd,"redact"))cmd_annotate(ctx,argc,argv,doc,1);
        else if(!strcmp(cmd,"draw"))cmd_draw(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"search"))cmd_search(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"fill"))cmd_fill(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"render"))cmd_render(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"protect"))cmd_password(ctx,argc,argv,doc,1);
        else if(!strcmp(cmd,"unprotect"))cmd_password(ctx,argc,argv,doc,0);
        else fz_throw(ctx,FZ_ERROR_ARGUMENT,"unknown command");
    }
    fz_always(ctx) pdf_drop_document(ctx,doc);
    fz_catch(ctx) fz_rethrow(ctx);
}
int main(int argc,char **argv) {
    if(argc<2 || !strcmp(argv[1],"--help") || !strcmp(argv[1],"help")){help();return argc<2?2:0;}
    fz_context *ctx=fz_new_context(NULL,NULL,FZ_STORE_DEFAULT);
    if(!ctx){fputs("unable to create MuPDF context\n",stderr);return 1;}
    fz_register_document_handlers(ctx);
    int code=0;
    fz_try(ctx) dispatch(ctx,argc,argv);
    fz_catch(ctx){fprintf(stderr,"pdf-tools-c: %s\n",fz_caught_message(ctx));code=1;}
    fz_drop_context(ctx);
    return code;
}

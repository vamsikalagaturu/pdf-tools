#include "pt_pdf.h"
#include "pt_ranges.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
         "  pages INPUT -o OUT [--order 3,1-2] [--remove 2,4] [--rotate PAGE:DEGREES] [--crop PAGE:X0,Y0,X1,Y1] [--crop-content [PAGES]]\n"
         "  annotate INPUT -o OUT --page N --type highlight|underline|strike|squiggly|note|text|rect|line\n"
         "           --rect X0,Y0,X1,Y1 [--text STRING] [--color R,G,B]\n"
         "  redact INPUT -o OUT --page N --rect X0,Y0,X1,Y1\n"
         "  draw INPUT -o OUT --page N --points X,Y;X,Y;... [--width POINTS]\n"
         "  search INPUT --text STRING\n"
         "  fill INPUT -o OUT --field NAME=VALUE (repeat --field)\n"
         "  flatten INPUT -o OUT [--dpi 200] (rasterize pages)\n"
         "  insert-image INPUT -o OUT --image PNG/JPG --page N --rect X0,Y0,X1,Y1\n"
         "  sign INPUT -o OUT --image SIGNATURE.PNG --page N --rect X0,Y0,X1,Y1\n"
         "  insert-text INPUT -o OUT --page N --at X,Y --text WORDS [--size 12]\n"
         "  edit-annotation INPUT -o OUT --page N --index ZERO_BASED [--remove | --text COMMENT]\n"
         "  redact-text INPUT -o OUT (--text PHRASE | --regex PATTERN) [--page N] [--case-sensitive]\n"
         "  synctex INPUT (--page N --at X,Y | --source TEX --line N)\n"
         "  inspect INPUT [--password P] (form fields, annotations)\n"
         "  scrub INPUT -o OUT (metadata, embedded files, scripts)\n"
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
static int ends_image(const char *s) {
    size_t n=strlen(s);
    return (n>=4 && (!strcasecmp(s+n-4,".png")||!strcasecmp(s+n-4,".jpg")||!strcasecmp(s+n-4,".bmp"))) ||
           (n>=5 && (!strcasecmp(s+n-5,".jpeg")||!strcasecmp(s+n-5,".tiff")));
}
/* A source may be a PDF, an image, or file.pdf:1-3 (same syntax as Python CLI). */
static void cmd_merge(fz_context *ctx,int argc,char **argv) {
    const char *out=out_path(argc,argv,NULL,ctx);
    pdf_document *dst=pdf_create_document(ctx), *src=NULL;
    int count=0;
    fz_try(ctx){
        for(int i=2;i<argc;i++){
            if(!strcmp(argv[i],"-o")){i++;continue;}
            const char *arg=argv[i];
            if(!strcmp(arg,out))fz_throw(ctx,FZ_ERROR_ARGUMENT,"output cannot be an input");
            if(ends_image(arg)) {pt_append_image_page(ctx,dst,arg);count++;continue;}
            char *path=strdup(arg);
            if(!path)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
            char *rangestr=strstr(path,".pdf:");
            if(!rangestr)rangestr=strstr(path,".PDF:");
            if(rangestr){rangestr[4]=0;rangestr+=5;}
            fz_try(ctx){
                src=pt_open(ctx,path,NULL);
                int n=pdf_count_pages(ctx,src);
                if(rangestr){
                    int *chosen=malloc((size_t)(n+8192)*sizeof(int));
                    if(!chosen)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
                    int k=pt_parse_pages(rangestr,n,chosen,(size_t)n+8192);
                    if(k<=0)fz_throw(ctx,FZ_ERROR_ARGUMENT,"invalid source page range: %s",arg);
                    for(int j=0;j<k;j++){pdf_graft_page(ctx,dst,-1,src,chosen[j]);count++;}
                    free(chosen);
                } else for(int j=0;j<n;j++){pdf_graft_page(ctx,dst,-1,src,j);count++;}
            }
            fz_always(ctx){pdf_drop_document(ctx,src);src=NULL;free(path);}
            fz_catch(ctx)fz_rethrow(ctx);
        }
        if(!count)fz_throw(ctx,FZ_ERROR_ARGUMENT,"merge needs at least one input");
        save_new(ctx,dst,out,NULL);
    }
    fz_always(ctx)pdf_drop_document(ctx,dst);
    fz_catch(ctx)fz_rethrow(ctx);
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
        /* Reopen and select pages in place: grafting to an empty PDF drops form fields. */
        pdf_document *dst=pt_open(ctx,argv[2],opt(argc,argv,"--password"));
        fz_try(ctx){
            pdf_rearrange_pages(ctx,dst,len,pages,PDF_CLEAN_STRUCTURE_DROP);
            char name[1024];snprintf(name,sizeof(name),"%s/part-%03d.pdf",dir,++part);
            pt_save(ctx,dst,name,NULL);
        }
        fz_always(ctx){pdf_drop_document(ctx,dst);free(pages);}
        fz_catch(ctx)fz_rethrow(ctx);
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
    pdf_rearrange_pages(ctx,src,count,order,PDF_CLEAN_STRUCTURE_DROP);
    pdf_document *dst=src;
    free(order);
    for(int i=3;i<argc-1;i++)if(!strcmp(argv[i],"--rotate")) {
        int p=0,d=0;
        if(sscanf(argv[++i],"%d:%d",&p,&d)!=2 || p<1 || p>count)
            fz_throw(ctx,FZ_ERROR_ARGUMENT,"expected --rotate PAGE:DEGREES");
        pt_rotate(ctx,dst,p-1,d);
    }
    for(int i=3;i<argc-1;i++)if(!strcmp(argv[i],"--crop")){
        int p=0;float a,b,c,d;
        if(sscanf(argv[++i],"%d:%f,%f,%f,%f",&p,&a,&b,&c,&d)!=5||p<1||p>count)
            fz_throw(ctx,FZ_ERROR_ARGUMENT,"expected --crop PAGE:X0,Y0,X1,Y1");
        pdf_page *pg=pdf_load_page(ctx,dst,p-1);
        fz_rect box={a,b,c,d};
        pdf_set_page_box(ctx,pg,FZ_CROP_BOX,box);
        pdf_drop_page(ctx,pg);
    }
    for(int i=3;i<argc;i++)if(!strcmp(argv[i],"--crop-content")){
        const char *range=(i+1<argc && argv[i+1][0]!='-')?argv[++i]:NULL;
        if(range){
            int *selected=malloc((size_t)count*sizeof(int));
            if(!selected)fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");
            int k=pt_parse_pages(range,count,selected,(size_t)count);
            if(k<1)fz_throw(ctx,FZ_ERROR_ARGUMENT,"invalid --crop-content page range");
            for(int j=0;j<k;j++)pt_crop_content(ctx,dst,selected[j],6);
            free(selected);
        }else for(int j=0;j<count;j++)pt_crop_content(ctx,dst,j,6);
    }
    save_new(ctx,dst,out_path(argc,argv,argv[2],ctx),NULL);
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
    if(redact)pt_scrub(ctx,doc);
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
/* Extra CLI commands: page-content edits and metadata. */
static void cmd_flatten(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    int dpi=number(opt(argc,argv,"--dpi"),200);
    pdf_document *copy=pt_flatten(ctx,doc,dpi);
    fz_try(ctx)save_new(ctx,copy,out_path(argc,argv,argv[2],ctx),NULL);
    fz_always(ctx)pdf_drop_document(ctx,copy);
    fz_catch(ctx)fz_rethrow(ctx);
}
static void cmd_place_image(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    const char *path=required(opt(argc,argv,"--image"),"--image FILE",ctx);
    fz_rect r=parse_rect(opt(argc,argv,"--rect"),ctx);
    pt_place_image(ctx,doc,page_no(argc,argv,ctx,doc),path,r);
    save_new(ctx,doc,out_path(argc,argv,argv[2],ctx),NULL);
}
static void cmd_insert_text(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    const char *words=required(opt(argc,argv,"--text"),"--text WORDS",ctx);
    const char *at=required(opt(argc,argv,"--at"),"--at X,Y",ctx);
    float x,y;char tail;
    if(sscanf(at," %f , %f %c",&x,&y,&tail)!=2 || !isfinite(x)||!isfinite(y))
        fz_throw(ctx,FZ_ERROR_ARGUMENT,"--at needs X,Y");
    float rgb[3]; color_of(opt(argc,argv,"--color"),rgb);
    pt_place_text(ctx,doc,page_no(argc,argv,ctx,doc),words,x,y,
                  (float)atof(opt(argc,argv,"--size")?opt(argc,argv,"--size"):"12"),rgb);
    save_new(ctx,doc,out_path(argc,argv,argv[2],ctx),NULL);
}
static void cmd_edit_annot(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    const char *idx=required(opt(argc,argv,"--index"),"--index N (0-based)",ctx);
    int index=atoi(idx);
    int remove=opt(argc,argv,"--remove")!=NULL;
    /* --remove is a flag, so allow it without consuming the next token. */
    for(int j=3;j<argc;j++)if(!strcmp(argv[j],"--remove"))remove=1;
    if(index<0)fz_throw(ctx,FZ_ERROR_ARGUMENT,"annotation index must be >=0");
    if(!remove && !opt(argc,argv,"--text"))fz_throw(ctx,FZ_ERROR_ARGUMENT,"--text required when editing an annotation");
    if(!pt_modify_annot(ctx,doc,page_no(argc,argv,ctx,doc),index,remove,opt(argc,argv,"--text")))
        fz_throw(ctx,FZ_ERROR_ARGUMENT,"annotation index not found");
    save_new(ctx,doc,out_path(argc,argv,argv[2],ctx),NULL);
}
static void json_string(const char *s) {
    putchar('"');
    if(!s)s="";
    for(const unsigned char *p=(const unsigned char*)s;*p;p++){
        if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}
        else if(*p<32)printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
/* Optional SyncTeX support via installed synctex command (without a shell).
 * A future mobile build should replace this adapter with the SyncTeX parser. */
#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
static char *synctex_capture(fz_context *ctx,char *const argv[]) {
    int p[2];if(pipe(p))fz_throw(ctx,FZ_ERROR_SYSTEM,"could not create SyncTeX pipe");
    pid_t child=fork();
    if(child<0){close(p[0]);close(p[1]);fz_throw(ctx,FZ_ERROR_SYSTEM,"could not fork SyncTeX");}
    if(child==0){
        close(p[0]);dup2(p[1],STDOUT_FILENO);close(p[1]);
        execvp("synctex",argv);_exit(127);
    }
    close(p[1]);
    size_t length=0,cap=65536;
    char *output=malloc(cap);
    if(!output){close(p[0]);waitpid(child,NULL,0);fz_throw(ctx,FZ_ERROR_SYSTEM,"out of memory");}
    for(;;){
        ssize_t n=read(p[0],output+length,cap-length-1);
        if(n<=0)break;
        length+=(size_t)n;
        if(length>=cap-1)break;
    }
    close(p[0]);int status=0;waitpid(child,&status,0);output[length]=0;
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0 || !strstr(output,"SyncTeX result begin")){
        free(output);fz_throw(ctx,FZ_ERROR_ARGUMENT,"SyncTeX failed (install synctex and compile with -synctex=1)");
    }
    return output;
}
static char *synctex_field(char *output,const char *key) {
    size_t n=strlen(key);
    for(char *p=output;*p;){
        if(!strncmp(p,key,n) && p[n]==':')return p+n+1;
        char *next=strchr(p,'\n');if(!next)break;p=next+1;
    }
    return NULL;
}
static void json_line(char *val){
    if(!val){fputs("null",stdout);return;}
    char *end=strchr(val,'\n');char original=end?*end:0;
    if(end)*end=0;json_string(val);if(end)*end=original;
}
static void cmd_synctex(fz_context *ctx,int argc,char **argv){
    const char *src=opt(argc,argv,"--source");
    const char *at=opt(argc,argv,"--at");
    const char *page=opt(argc,argv,"--page");
    const char *line=opt(argc,argv,"--line");
    const char *pdf=argv[2];
    char where[8192];
    char *const *call;
    char *argv_view[]={"synctex","view","-i",where,"-o",(char*)pdf,NULL};
    char *argv_edit[]={"synctex","edit","-o",where,NULL};
    if(src && line && !at && !page){
        if(strlen(src)>8000)fz_throw(ctx,FZ_ERROR_ARGUMENT,"source path too long");
        snprintf(where,sizeof(where),"%s:0:%s",line,src);
        call=argv_view;
    }else if(!src && !line && page && at){
        if(strlen(pdf)>8000)fz_throw(ctx,FZ_ERROR_ARGUMENT,"PDF path too long");
        char a[128];if(strlen(at)>=sizeof(a))fz_throw(ctx,FZ_ERROR_ARGUMENT,"--at too long");
        strcpy(a,at);for(char *p=a;*p;p++)if(*p==',')*p=':';
        snprintf(where,sizeof(where),"%s:%s:%s",page,a,pdf);
        call=argv_edit;
    }else fz_throw(ctx,FZ_ERROR_ARGUMENT,"SyncTeX needs --source FILE --line N or --page N --at X,Y");
    char *result=synctex_capture(ctx,(char *const *)call);
    if(src){
        char *p=synctex_field(result,"Page"),*x=synctex_field(result,"h"),*y=synctex_field(result,"v");
        char *w=synctex_field(result,"W"),*h=synctex_field(result,"H");
        if(!p || !x || !y || !w || !h){free(result);fz_throw(ctx,FZ_ERROR_ARGUMENT,"SyncTeX has no match");}
        float px=atof(x),py=atof(y),pw=atof(w),ph=atof(h);
        printf("{\"page\":%d,\"rects\":[[%.3f,%.3f,%.3f,%.3f]]}\n",atoi(p),px,py-ph,px+pw,py);
    }else{
        char *source=synctex_field(result,"Input"),*ln=synctex_field(result,"Line");
        if(!source||!ln){free(result);fz_throw(ctx,FZ_ERROR_ARGUMENT,"SyncTeX has no match");}
        printf("{\"source\":");json_line(source);printf(",\"line\":%d}\n",atoi(ln));
    }
    free(result);
}
#else
static void cmd_synctex(fz_context *ctx,int argc,char **argv){
    (void)argc;(void)argv;fz_throw(ctx,FZ_ERROR_ARGUMENT,"SyncTeX CLI adapter not supported on this platform");
}
#endif

static void cmd_inspect(fz_context *ctx,pdf_document *doc) {
    int count=pdf_count_pages(ctx,doc);
    printf("{\"pages\":%d,\"fields\":[",count);
    int first=1;
    for(int i=0;i<count;i++){
        pdf_page *pg=pdf_load_page(ctx,doc,i);
        for(pdf_annot *w=pdf_first_widget(ctx,pg);w;w=pdf_next_widget(ctx,w)){
            pdf_obj *field=pdf_annot_obj(ctx,w);
            char *name=pdf_load_field_name(ctx,field);
            pdf_obj *value=pdf_dict_get_inheritable(ctx,field,PDF_NAME(V));
            printf("%s{\"page\":%d,\"name\":",first?"":",",i+1);json_string(name);
            printf(",\"value\":");json_string(pdf_to_text_string(ctx,value));putchar('}');
            first=0;fz_free(ctx,name);
        }
        pdf_drop_page(ctx,pg);
    }
    printf("],\"annotations\":[");first=1;
    for(int i=0;i<count;i++){
        pdf_page *pg=pdf_load_page(ctx,doc,i);
        int index=0;
        for(pdf_annot *a=pdf_first_annot(ctx,pg);a;a=pdf_next_annot(ctx,a)){
            fz_rect r=pdf_bound_annot(ctx,a);
            printf("%s{\"page\":%d,\"index\":%d,\"type\":",first?"":",",i+1,index++);
            json_string(pdf_string_from_annot_type(ctx,pdf_annot_type(ctx,a)));
            printf(",\"rect\":[%.2f,%.2f,%.2f,%.2f],\"text\":",r.x0,r.y0,r.x1,r.y1);
            json_string(pdf_annot_contents(ctx,a));putchar('}');first=0;
        }
        pdf_drop_page(ctx,pg);
    }
    puts("]}");
}
static void cmd_scrub(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    pt_scrub(ctx,doc);
    save_new(ctx,doc,out_path(argc,argv,argv[2],ctx),"garbage=deduplicate,compress");
}
static void cmd_redact_text(fz_context *ctx,int argc,char **argv,pdf_document *doc) {
    int page=-1;
    if(opt(argc,argv,"--page"))page=page_no(argc,argv,ctx,doc);
    const char *pattern=opt(argc,argv,"--regex");
    const char *phrase=opt(argc,argv,"--text");
    if((pattern!=NULL)==(phrase!=NULL))fz_throw(ctx,FZ_ERROR_ARGUMENT,"provide exactly one of --text or --regex");
    int hits;
    if(pattern){
#ifdef PT_USE_PCRE2
        int sensitive=0;for(int j=3;j<argc;j++)if(!strcmp(argv[j],"--case-sensitive"))sensitive=1;
        hits=pt_redact_regex(ctx,doc,pattern,page,sensitive);
#else
        fz_throw(ctx,FZ_ERROR_ARGUMENT,"regex support unavailable: rebuild with PCRE2");
#endif
    }else hits=pt_redact_text(ctx,doc,phrase,page);
    if(!hits)fz_throw(ctx,FZ_ERROR_ARGUMENT,"no matching text found; no file written");
    pt_scrub(ctx,doc);
    pt_save(ctx,doc,out_path(argc,argv,argv[2],ctx),"garbage=deduplicate,compress");
    printf("{\"redactions\":%d,\"ok\":true}\n",hits);
}

static void dispatch(fz_context *ctx,int argc,char **argv) {
    const char *cmd=argv[1];
    if(!strcmp(cmd,"merge")){cmd_merge(ctx,argc,argv);return;}
    if(!strcmp(cmd,"synctex")){if(argc<3)fz_throw(ctx,FZ_ERROR_ARGUMENT,"missing INPUT");cmd_synctex(ctx,argc,argv);return;}
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
        else if(!strcmp(cmd,"flatten"))cmd_flatten(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"insert-image") || !strcmp(cmd,"sign"))cmd_place_image(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"insert-text"))cmd_insert_text(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"edit-annotation"))cmd_edit_annot(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"inspect"))cmd_inspect(ctx,doc);
        else if(!strcmp(cmd,"scrub"))cmd_scrub(ctx,argc,argv,doc);
        else if(!strcmp(cmd,"redact-text"))cmd_redact_text(ctx,argc,argv,doc);
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

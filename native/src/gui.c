/* Native raylib canvas for the MuPDF engine. Platform-specific file pickers are not yet implemented. */
#include "pt_pdf.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TOP 54
#define BOTTOM 32
#define MAX_STROKE 8192

typedef enum { HAND, HIGHLIGHT, UNDERLINE, REDACT, RECT, INK, NOTE, TEXT } mode_t;
static const char *labels[] = { "Hand [H]", "Mark [M]", "Underline [U]", "Redact [R]", "Box [B]", "Pen [P]", "Note [N]", "Text [T]" };

static fz_context *ctx;
static pdf_document *doc;
static Texture2D page_texture;
static int texture_ready;
static int current_page, total_pages;
static float zoom=1.5f, offset_y=0, offset_x=0;
static mode_t tool=HAND;
static int dragging, in_text;
static fz_point down_pt;
static fz_point pen_pts[MAX_STROKE];
static int pen_count;
static char input_text[512];
static fz_point text_point;
static char status[512]="Open a PDF, choose a tool, Ctrl+S to Save As";
static const float mark_color[3]={1.0f, 0.8f, 0.15f};
static const float pen_color[3]={0.1f, 0.3f, 0.85f};
static const float black[3]={0,0,0};
static int dirty;

static void message(const char *msg) { snprintf(status,sizeof(status),"%s",msg); }
static void unload_page(void) { if(texture_ready) { UnloadTexture(page_texture); texture_ready=0; } }
static int load_page(void) {
    unload_page();
    int good=1;
    fz_pixmap *pix=NULL;
    fz_try(ctx) {
        pix=pt_render(ctx,doc,current_page,zoom);
        Image image={0};
        image.data=pix->samples;
        image.width=pix->w;
        image.height=pix->h;
        image.mipmaps=1;
        image.format=pix->n==4 ? PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 : PIXELFORMAT_UNCOMPRESSED_R8G8B8;
        page_texture=LoadTextureFromImage(image);
        texture_ready=page_texture.id!=0;
    }
    fz_always(ctx) { fz_drop_pixmap(ctx,pix); }
    fz_catch(ctx) { message(fz_caught_message(ctx));good=0; }
    return good;
}
static Vector2 origin(void) {
    return (Vector2){ (GetScreenWidth() - (float)page_texture.width)/2 + offset_x,
                      TOP+24+offset_y };
}
static Vector2 screen_point(fz_point p) {
    Vector2 base=origin();
    return (Vector2){base.x+p.x*zoom,base.y+p.y*zoom};
}
static fz_point pdf_point(Vector2 p) {
    Vector2 base=origin();
    return (fz_point){(p.x-base.x)/zoom,(p.y-base.y)/zoom};
}
static int inside_page(Vector2 p) {
    Vector2 base=origin();
    return texture_ready && p.x>=base.x && p.y>=base.y &&
           p.x<base.x+page_texture.width && p.y<base.y+page_texture.height &&
           p.y>=TOP && p.y<GetScreenHeight()-BOTTOM;
}
static void page_change(int dir) {
    int next=current_page+dir;
    if(next<0||next>=total_pages)return;
    current_page=next;offset_y=0;offset_x=0;load_page();
}
static void set_zoom(float factor) {
    zoom=fmaxf(0.4f,fminf(3.0f,zoom*factor));
    load_page();
}
static void edit_rect(fz_point a,fz_point b) {
    fz_rect box={a.x,a.y,b.x,b.y};
    pt_tool type=PT_RECT;
    const float *color=mark_color;
    if(tool==HIGHLIGHT)type=PT_HIGHLIGHT;
    else if(tool==UNDERLINE)type=PT_UNDERLINE;
    else if(tool==REDACT){type=PT_REDACT;color=black;}
    else if(tool==RECT){type=PT_RECT;color=pen_color;}
    fz_try(ctx) {
        pt_add_rect(ctx,doc,current_page,type,box,NULL,color);
        dirty=1;
        message(type==PT_REDACT ? "Redaction applied; review before sharing" : "Annotation added");
        load_page();
    }
    fz_catch(ctx)message(fz_caught_message(ctx));
}
static void edit_pen(void) {
    if(pen_count<2)return;
    fz_try(ctx) {
        pt_add_stroke(ctx,doc,current_page,pen_pts,pen_count,2.2f,pen_color);
        dirty=1;message("Ink annotation added");load_page();
    }
    fz_catch(ctx)message(fz_caught_message(ctx));
}
static void edit_text(void) {
    if(!input_text[0])return;
    fz_rect box={text_point.x,text_point.y,text_point.x+270,text_point.y+48};
    fz_try(ctx) {
        pt_add_rect(ctx,doc,current_page,tool==TEXT?PT_TEXT:PT_NOTE,box,input_text,black);
        dirty=1;message("Text annotation placed");load_page();
    }
    fz_catch(ctx)message(fz_caught_message(ctx));
}
static void save_document(const char *path) {
    fz_try(ctx) {
        pt_save(ctx,doc,path,NULL);
        dirty=0;snprintf(status,sizeof(status),"Saved: %.400s",path);
    }
    fz_catch(ctx)message(fz_caught_message(ctx));
}
static int toolbar_button(int index, int x) {
    Rectangle r={(float)x,8,108,37};
    int active=tool==index;
    DrawRectangleRounded(r,0.16f,4,active?(Color){52,105,170,255}:(Color){55,58,64,255});
    DrawText(labels[index],x+7,19,13,RAYWHITE);
    if(CheckCollisionPointRec(GetMousePosition(),r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if(in_text)in_text=0;
        tool=(mode_t)index;return 1;
    }
    return 0;
}
static void shortcuts(void) {
    if(in_text)return;
    if(IsKeyPressed(KEY_H))tool=HAND;
    if(IsKeyPressed(KEY_M))tool=HIGHLIGHT;
    if(IsKeyPressed(KEY_U))tool=UNDERLINE;
    if(IsKeyPressed(KEY_R))tool=REDACT;
    if(IsKeyPressed(KEY_B))tool=RECT;
    if(IsKeyPressed(KEY_P))tool=INK;
    if(IsKeyPressed(KEY_N))tool=NOTE;
    if(IsKeyPressed(KEY_T))tool=TEXT;
    if(IsKeyPressed(KEY_PAGE_DOWN)||IsKeyPressed(KEY_RIGHT))page_change(+1);
    if(IsKeyPressed(KEY_PAGE_UP)||IsKeyPressed(KEY_LEFT))page_change(-1);
    if(IsKeyPressed(KEY_EQUAL))set_zoom(1.15f);
    if(IsKeyPressed(KEY_MINUS))set_zoom(1/1.15f);
}
int main(int argc,char **argv) {
    if(argc<2){fprintf(stderr,"Usage: pdf-tools-gui INPUT.pdf [OUTPUT.pdf]\n");return 2;}
    const char *input=argv[1];
    char output[4096];
    if(argc>=3)snprintf(output,sizeof(output),"%s",argv[2]);
    else snprintf(output,sizeof(output),"%s.edited.pdf",input);
    if(!strcmp(input,output)) {fputs("Output must differ from input\n",stderr);return 2;}
    ctx=fz_new_context(NULL,NULL,FZ_STORE_DEFAULT);
    if(!ctx)return 1;
    fz_register_document_handlers(ctx);
    int opened=1;
    fz_try(ctx) {doc=pt_open(ctx,input,NULL);total_pages=pdf_count_pages(ctx,doc);}
    fz_catch(ctx){fprintf(stderr,"PDF: %s\n",fz_caught_message(ctx));opened=0;}
    if(!opened) {fz_drop_context(ctx);return 1;}
    SetConfigFlags(FLAG_WINDOW_RESIZABLE|FLAG_MSAA_4X_HINT);
    InitWindow(1260,860,"PDF Tools C - MuPDF + raylib");
    SetTargetFPS(60);
    load_page();
    while(!WindowShouldClose()) {
        Vector2 mouse=GetMousePosition();
        if((IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_S))
            save_document(output);
        shortcuts();
        if(IsKeyPressed(KEY_ESCAPE)) {in_text=0;dragging=0;pen_count=0;}
        if(in_text) {
            int ch;
            while((ch=GetCharPressed())>0) {
                size_t n=strlen(input_text);
                if(ch>=32&&ch<127&&n+1<sizeof(input_text)){
                    input_text[n]=(char)ch;input_text[n+1]=0;
                }
            }
            if(IsKeyPressed(KEY_BACKSPACE)){
                size_t n=strlen(input_text);if(n)input_text[n-1]=0;
            }
            if(IsKeyPressed(KEY_ENTER)) {edit_text();in_text=0;}
        }
        if(!in_text && mouse.y>=TOP) {
            float wheel=GetMouseWheelMove();
            if(wheel) {
                if(IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL))set_zoom(wheel>0?1.14f:1/1.14f);
                else offset_y+=wheel*55;
            }
            if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && inside_page(mouse)) {
                down_pt=pdf_point(mouse);
                if(tool==TEXT || tool==NOTE) {
                    text_point=down_pt;input_text[0]=0;in_text=1;
                } else {
                    dragging=1;pen_count=0;
                    if(tool==INK)pen_pts[pen_count++]=down_pt;
                }
            }
            if(dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && tool==HAND){
                Vector2 delta=GetMouseDelta();offset_x+=delta.x;offset_y+=delta.y;
            }
            if(dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && tool==INK && pen_count<MAX_STROKE) {
                fz_point p=pdf_point(mouse);if(pen_count==0 || hypotf(p.x-pen_pts[pen_count-1].x,p.y-pen_pts[pen_count-1].y)>.8f)
                    pen_pts[pen_count++]=p;
            }
            if(dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)){
                dragging=0;
                if(tool==INK)edit_pen();
                else if(tool!=HAND)edit_rect(down_pt,pdf_point(mouse));
            }
        }
        BeginDrawing();ClearBackground((Color){30,32,37,255});
        DrawRectangle(0,0,GetScreenWidth(),TOP,(Color){40,42,48,255});
        for(int i=0;i<8;i++)toolbar_button(i,9+i*111);
        DrawText(TextFormat("%d / %d   Zoom %.0f%%",current_page+1,total_pages,zoom*100),905,18,17,RAYWHITE);
        BeginScissorMode(0,TOP,GetScreenWidth(),GetScreenHeight()-TOP-BOTTOM);
        if(texture_ready){
            Vector2 base=origin();
            DrawRectangle((int)base.x-2,(int)base.y-2,page_texture.width+4,page_texture.height+4,(Color){90,92,99,255});
            DrawTextureV(page_texture,base,WHITE);
        }
        if(dragging && tool!=HAND) {
            if(tool==INK) {
                for(int i=1;i<pen_count;i++)DrawLineEx(screen_point(pen_pts[i-1]),screen_point(pen_pts[i]),2.2f*zoom,BLUE);
            }else{
                Vector2 start=screen_point(down_pt);
                float x=fminf(start.x,mouse.x),y=fminf(start.y,mouse.y);
                DrawRectangleLinesEx((Rectangle){x,y,fabsf(mouse.x-start.x),fabsf(mouse.y-start.y)},2,RED);
            }
        }
        EndScissorMode();
        if(in_text) {
            Rectangle r={8,(float)GetScreenHeight()-80,(float)GetScreenWidth()-16,42};
            DrawRectangleRec(r,(Color){50,62,77,250});
            DrawText(TextFormat("Enter text, then Return: %s_",input_text),18,GetScreenHeight()-68,18,WHITE);
        }
        DrawRectangle(0,GetScreenHeight()-BOTTOM,GetScreenWidth(),BOTTOM,(Color){40,42,48,255});
        DrawText(TextFormat("%s%s | Save As: %s",status,dirty?"  *unsaved*":"",output),10,GetScreenHeight()-23,15,RAYWHITE);
        EndDrawing();
    }
    unload_page();CloseWindow();pdf_drop_document(ctx,doc);fz_drop_context(ctx);
    return 0;
}

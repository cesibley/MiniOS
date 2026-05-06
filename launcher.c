#include <efi.h>
#include <efilib.h>
#include "watchdog.h"
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC(sz) AllocatePool((UINTN)(sz))
#define STBI_REALLOC_SIZED(p, oldsz, newsz) ReallocatePool((p), (UINTN)(oldsz), (UINTN)(newsz))
#define STBI_FREE(p) FreePool((p))
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int tt_ifloor(double x){ int i=(int)x; return (x<(double)i)?(i-1):i; }
static int tt_iceil(double x){ int i=(int)x; return (x>(double)i)?(i+1):i; }
#define STBTT_ifloor(x) tt_ifloor((x))
#define STBTT_iceil(x)  tt_iceil((x))
#define STBTT_sqrt(x)   (x)
#define STBTT_pow(x,y)  (x)
#define STBTT_fmod(x,y) ((void)(x),(void)(y),0.0)
#define STBTT_cos(x)    ((void)(x),1.0)
#define STBTT_acos(x)   ((void)(x),0.0)
#define STBTT_fabs(x)   (((x) < 0) ? -(x) : (x))
#define STBTT_malloc(sz, u) ((void)(u), AllocatePool((UINTN)(sz)))
#define STBTT_free(p, u) ((void)(u), FreePool((p)))
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

typedef struct { BOOLEAN ok; UINT8 *data; UINTN size; stbtt_fontinfo font; float scale; int ascent; int descent; int line_gap; } TT_FONT;

#define MAX_ITEMS 256
#define LAUNCHER_NAME_MAX 128
#define META_BUF 1024
#define ICON_PX 64

typedef struct { CHAR16 name[LAUNCHER_NAME_MAX]; BOOLEAN is_dir; CHAR16 icon[64]; } ITEM;

typedef struct {
    BOOLEAN ok;
    UINTN w;
    UINTN h;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *px;
} ICON_IMAGE;

typedef struct {
    BOOLEAN ok;
    UINTN w;
    UINTN h;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *px;
} POINTER_IMAGE;

typedef struct { BOOLEAN use_backbuf; EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf; UINTN w; UINTN h; } RENDER_TARGET;

static VOID rt_read(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, UINTN x, UINTN y, UINTN w, UINTN h, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *out){
    UINTN row;
    if(!w||!h||out==NULL) return;
    if(rt && rt->use_backbuf && rt->buf){ for(row=0; row<h; row++) CopyMem(out + row*w, rt->buf + (y+row)*rt->w + x, w*sizeof(*out)); }
    else uefi_call_wrapper(g->Blt,10,g,out,EfiBltVideoToBltBuffer,x,y,0,0,w,h,0);
}
static VOID rt_write(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, UINTN x, UINTN y, UINTN w, UINTN h, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *in){
    UINTN row;
    if(!w||!h||in==NULL) return;
    if(rt && rt->use_backbuf && rt->buf){ for(row=0; row<h; row++) CopyMem(rt->buf + (y+row)*rt->w + x, in + row*w, w*sizeof(*in)); }
    else uefi_call_wrapper(g->Blt,10,g,in,EfiBltBufferToVideo,0,0,x,y,w,h,0);
}

static VOID fill(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, UINTN x, UINTN y, UINTN w, UINTN h, EFI_GRAPHICS_OUTPUT_BLT_PIXEL c){ UINTN row,col; if(!w||!h) return; if(rt&&rt->use_backbuf&&rt->buf){ for(row=0;row<h;row++) for(col=0;col<w;col++) rt->buf[(y+row)*rt->w+(x+col)]=c; } else uefi_call_wrapper(g->Blt,10,g,&c,EfiBltVideoFill,0,0,x,y,w,h,0);} 
static BOOLEAN contains_ci(const CHAR8 *s, const CHAR8 *pat){ UINTN i,j; for(i=0;s&&s[i];i++){ for(j=0;pat[j]&&s[i+j];j++){ CHAR8 a=s[i+j],b=pat[j]; if(a>='a'&&a<='z')a-=32; if(b>='a'&&b<='z')b-=32; if(a!=b) break;} if(!pat[j]) return TRUE;} return FALSE; }
static VOID join_path(CHAR16 *out, UINTN out_sz, CHAR16 *base, CHAR16 *name){ if(StrCmp(base,L"\\")==0) SPrint(out,out_sz,L"\\%s",name); else SPrint(out,out_sz,L"%s\\%s",base,name); }
static VOID make_meta_path(CHAR16 *out, UINTN out_sz, CHAR16 *name){ SPrint(out,out_sz,L"\\.meta\\%s.meta",name); }
#ifndef SCAN_PRINT
#define SCAN_PRINT 0x63
#endif
static EFI_STATUS open_root(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, EFI_FILE_HANDLE *root);
static EFI_STATUS save_screenshot_bmp(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, EFI_GRAPHICS_OUTPUT_PROTOCOL *g){
    EFI_FILE_HANDLE root,f,mf; EFI_STATUS s; CHAR16 name[64]=L"\\screenshot", meta_path[128];
    UINTN w=g->Mode->Info->HorizontalResolution,h=g->Mode->Info->VerticalResolution,row,col,pixsz=w*h*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
    UINTN headersz=54, datasz=w*h*3, filesz=headersz+datasz, bsz;
    UINT8 *filebuf,*p; EFI_GRAPHICS_OUTPUT_BLT_PIXEL *px;
    CHAR8 meta_txt[]="TYPE: Bitmap\nDESC: Screen Capture\nHANDLER: view\n";
    s=open_root(ih,st,&root); if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(root->Open,5,root,&f,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return s; }
    px=AllocatePool(pixsz); filebuf=AllocateZeroPool(filesz); if(px==NULL||filebuf==NULL){ if(px)FreePool(px); if(filebuf)FreePool(filebuf); uefi_call_wrapper(f->Close,1,f); uefi_call_wrapper(root->Close,1,root); return EFI_OUT_OF_RESOURCES; }
    s=uefi_call_wrapper(g->Blt,10,g,px,EfiBltVideoToBltBuffer,0,0,0,0,w,h,0); if(EFI_ERROR(s)){ FreePool(px); FreePool(filebuf); uefi_call_wrapper(f->Close,1,f); uefi_call_wrapper(root->Close,1,root); return s; }
    filebuf[0]='B'; filebuf[1]='M';
    filebuf[2]=(UINT8)(filesz); filebuf[3]=(UINT8)(filesz>>8); filebuf[4]=(UINT8)(filesz>>16); filebuf[5]=(UINT8)(filesz>>24);
    filebuf[10]=54; filebuf[14]=40;
    filebuf[18]=(UINT8)(w); filebuf[19]=(UINT8)(w>>8); filebuf[20]=(UINT8)(w>>16); filebuf[21]=(UINT8)(w>>24);
    filebuf[22]=(UINT8)(h); filebuf[23]=(UINT8)(h>>8); filebuf[24]=(UINT8)(h>>16); filebuf[25]=(UINT8)(h>>24);
    filebuf[26]=1; filebuf[28]=24;
    p=filebuf+54;
    for(row=0;row<h;row++){ UINTN src_row=h-1-row; for(col=0;col<w;col++){ EFI_GRAPHICS_OUTPUT_BLT_PIXEL q=px[src_row*w+col]; *p++=q.Blue; *p++=q.Green; *p++=q.Red; } }
    bsz=filesz; s=uefi_call_wrapper(f->Write,3,f,&bsz,filebuf);
    uefi_call_wrapper(f->Close,1,f);
    if(!EFI_ERROR(s)){
        make_meta_path(meta_path,sizeof(meta_path),L"screenshot");
        s=uefi_call_wrapper(root->Open,5,root,&mf,meta_path,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
        if(!EFI_ERROR(s)){ bsz=sizeof(meta_txt)-1; s=uefi_call_wrapper(mf->Write,3,mf,&bsz,meta_txt); uefi_call_wrapper(mf->Close,1,mf); }
    }
    FreePool(px); FreePool(filebuf); uefi_call_wrapper(root->Close,1,root); return s;
}
static BOOLEAN invalid_name_char(CHAR16 c){ return (c==L'\\'||c==L'/'||c==L':'||c==L'*'||c==L'?'||c==L'\"'||c==L'<'||c==L'>'||c==L'|'); }
static BOOLEAN valid_rename_name(CHAR16 *n){ UINTN i=0; if(n==NULL||n[0]==0) return FALSE; while(n[i]){ if(invalid_name_char(n[i])) return FALSE; i++; } return TRUE; }

static EFI_STATUS delete_selected_with_meta(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *cwd, CHAR16 *name){
    EFI_FILE_HANDLE root,dir,item,meta;
    EFI_STATUS s;
    CHAR16 mp[320];
    s=open_root(ih,st,&root); if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(root->Open,5,root,&dir,cwd,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
    if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return s; }
    s=uefi_call_wrapper(dir->Open,5,dir,&item,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
    if(EFI_ERROR(s)){ uefi_call_wrapper(dir->Close,1,dir); uefi_call_wrapper(root->Close,1,root); return s; }
    s=uefi_call_wrapper(item->Delete,1,item);
    if(EFI_ERROR(s)){ uefi_call_wrapper(dir->Close,1,dir); uefi_call_wrapper(root->Close,1,root); return s; }
    make_meta_path(mp,sizeof(mp),name);
    s=uefi_call_wrapper(root->Open,5,root,&meta,mp,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
    if(!EFI_ERROR(s)) uefi_call_wrapper(meta->Delete,1,meta);
    uefi_call_wrapper(dir->Close,1,dir);
    uefi_call_wrapper(root->Close,1,root);
    return EFI_SUCCESS;
}
static EFI_STATUS rename_in_dir(EFI_FILE_HANDLE dir, CHAR16 *old_name, CHAR16 *new_name){
    EFI_FILE_HANDLE f; EFI_FILE_INFO *fi; UINTN sz=0,min_sz; EFI_STATUS s;
    s=uefi_call_wrapper(dir->Open,5,dir,&f,new_name,EFI_FILE_MODE_READ,0);
    if(!EFI_ERROR(s)){ uefi_call_wrapper(f->Close,1,f); return EFI_ACCESS_DENIED; }
    s=uefi_call_wrapper(dir->Open,5,dir,&f,old_name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0); if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(f->GetInfo,4,f,&GenericFileInfo,&sz,NULL);
    if(s!=EFI_BUFFER_TOO_SMALL){ uefi_call_wrapper(f->Close,1,f); return EFI_DEVICE_ERROR; }
    min_sz=SIZE_OF_EFI_FILE_INFO + (StrLen(new_name)+1)*sizeof(CHAR16);
    if(sz<min_sz) sz=min_sz;
    fi=AllocateZeroPool(sz); if(fi==NULL){ uefi_call_wrapper(f->Close,1,f); return EFI_OUT_OF_RESOURCES; }
    s=uefi_call_wrapper(f->GetInfo,4,f,&GenericFileInfo,&sz,fi);
    if(EFI_ERROR(s)){ FreePool(fi); uefi_call_wrapper(f->Close,1,f); return s; }
    StrCpy(fi->FileName,new_name);
    fi->Size=sz;
    s=uefi_call_wrapper(f->SetInfo,4,f,&GenericFileInfo,sz,fi);
    FreePool(fi); uefi_call_wrapper(f->Close,1,f); return s;
}
static EFI_STATUS rename_selected_with_meta(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *cwd, CHAR16 *old_name, CHAR16 *new_name){
    EFI_FILE_HANDLE root,dir; EFI_STATUS s; CHAR16 om[320],nm[320];
    if(!valid_rename_name(new_name)) return EFI_INVALID_PARAMETER;
    if(StrCmp(old_name,new_name)==0) return EFI_SUCCESS;
    s=open_root(ih,st,&root); if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(root->Open,5,root,&dir,cwd,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
    if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return s; }
    s=rename_in_dir(dir,old_name,new_name);
    if(!EFI_ERROR(s)){
        make_meta_path(om,sizeof(om),old_name); make_meta_path(nm,sizeof(nm),new_name);
        (void)rename_in_dir(root,om,nm);
    }
    uefi_call_wrapper(dir->Close,1,dir); uefi_call_wrapper(root->Close,1,root); return s;
}

static EFI_STATUS open_root(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, EFI_FILE_HANDLE *root){ EFI_LOADED_IMAGE *li; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs; EFI_STATUS s; s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,ih,&LoadedImageProtocol,(VOID**)&li); if(EFI_ERROR(s)) return s; s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,li->DeviceHandle,&FileSystemProtocol,(VOID**)&fs); if(EFI_ERROR(s)) return s; return uefi_call_wrapper(fs->OpenVolume,2,fs,root);} 

static VOID pick_icon(EFI_FILE_HANDLE root, CHAR16 *name, BOOLEAN is_dir, CHAR16 *icon){ CHAR16 meta_path[220]; EFI_FILE_HANDLE mf; UINT8 buf[META_BUF]; UINTN sz=META_BUF-1; EFI_STATUS s; const CHAR8 *txt=(const CHAR8*)buf; if(is_dir){ StrCpy(icon,L"folder.ico"); return; } SPrint(meta_path,sizeof(meta_path),L"\\.meta\\%s.meta",name); s=uefi_call_wrapper(root->Open,5,root,&mf,meta_path,EFI_FILE_MODE_READ,0); if(EFI_ERROR(s)){ StrCpy(icon,L"file.ico"); return; } s=uefi_call_wrapper(mf->Read,3,mf,&sz,buf); uefi_call_wrapper(mf->Close,1,mf); if(EFI_ERROR(s)){ StrCpy(icon,L"file.ico"); return; } buf[sz]=0; if(contains_ci(txt,(const CHAR8 *)"TYPE: Program")||contains_ci(txt,(const CHAR8 *)"TYPE: PRGM")) StrCpy(icon,L"program.ico");
 else if(contains_ci(txt,(const CHAR8 *)"TYPE: bitmap")||contains_ci(txt,(const CHAR8 *)"TYPE: bmp")||contains_ci(txt,(const CHAR8 *)"TYPE: gif")||contains_ci(txt,(const CHAR8 *)"TYPE: jpeg")||contains_ci(txt,(const CHAR8 *)"TYPE: jpg")) StrCpy(icon,L"graphic.ico");
 else StrCpy(icon,L"file.ico"); }

static UINTN load_items(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, ITEM *it, CHAR16 *cwd){ EFI_FILE_HANDLE root,dir; EFI_STATUS s; UINTN n=0,bsz; UINT8 entbuf[512]; BOOLEAN has_parent=FALSE; s=open_root(ih,st,&root); if(EFI_ERROR(s)) return 0; s=uefi_call_wrapper(root->Open,5,root,&dir,cwd,EFI_FILE_MODE_READ,0); if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return 0;} while(n<MAX_ITEMS){ EFI_FILE_INFO *fi=(EFI_FILE_INFO*)entbuf; bsz=sizeof(entbuf); s=uefi_call_wrapper(dir->Read,3,dir,&bsz,fi); if(EFI_ERROR(s)||bsz==0) break; if(StrCmp(fi->FileName,L"..")==0){ has_parent=TRUE; continue; } if(fi->FileName[0]==L'.') continue; StrnCpy(it[n].name,fi->FileName,LAUNCHER_NAME_MAX-1); it[n].name[LAUNCHER_NAME_MAX-1]=0; it[n].is_dir=(fi->Attribute&EFI_FILE_DIRECTORY)?TRUE:FALSE; pick_icon(root,it[n].name,it[n].is_dir,it[n].icon); n++; } if(has_parent && n<MAX_ITEMS){ UINTN i; for(i=n;i>0;i--) it[i]=it[i-1]; StrCpy(it[0].name,L".."); it[0].is_dir=TRUE; StrCpy(it[0].icon,L"folder.ico"); n++; } uefi_call_wrapper(dir->Close,1,dir); uefi_call_wrapper(root->Close,1,root); return n; }

static EFI_STATUS run_item(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *cwd, CHAR16 *name){ EFI_LOADED_IMAGE *li; EFI_DEVICE_PATH *dp; EFI_HANDLE h=NULL; EFI_STATUS s; CHAR16 path[180]; s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,ih,&LoadedImageProtocol,(VOID**)&li); if(EFI_ERROR(s)) return s; join_path(path,sizeof(path),cwd,name); dp=FileDevicePath(li->DeviceHandle,path); if(dp==NULL) return EFI_NOT_FOUND; s=uefi_call_wrapper(st->BootServices->LoadImage,6,FALSE,ih,dp,NULL,0,&h); if(EFI_ERROR(s)) return s; return uefi_call_wrapper(st->BootServices->StartImage,3,h,NULL,NULL); }
static EFI_STATUS run_item_with_args(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *cwd, CHAR16 *prog, CHAR16 *arg){ EFI_LOADED_IMAGE *li,*child=NULL; EFI_DEVICE_PATH *dp; EFI_HANDLE h=NULL; EFI_STATUS s; CHAR16 path[180]; s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,ih,&LoadedImageProtocol,(VOID**)&li); if(EFI_ERROR(s)) return s; join_path(path,sizeof(path),cwd,prog); dp=FileDevicePath(li->DeviceHandle,path); if(dp==NULL) return EFI_NOT_FOUND; s=uefi_call_wrapper(st->BootServices->LoadImage,6,FALSE,ih,dp,NULL,0,&h); if(EFI_ERROR(s)) return s; if(arg!=NULL && arg[0]!=0){ s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,h,&LoadedImageProtocol,(VOID**)&child); if(!EFI_ERROR(s) && child!=NULL){ child->LoadOptions=(VOID*)arg; child->LoadOptionsSize=(UINT32)((StrLen(arg)+1)*sizeof(CHAR16)); } } return uefi_call_wrapper(st->BootServices->StartImage,3,h,NULL,NULL); }
static BOOLEAN is_program_type(const CHAR8 *t){ return t && (contains_ci(t,(const CHAR8*)"PROGRAM") || contains_ci(t,(const CHAR8*)"PRGM")); }
static VOID ascii_to_char16(const CHAR8 *in, CHAR16 *out, UINTN out_len){ UINTN i=0; if(out_len==0) return; while(in&&in[i]&&i+1<out_len){ out[i]=(CHAR16)(in[i]&0xFF); i++; } out[i]=0; }
static VOID read_meta_type_handler(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *name, CHAR8 *type, UINTN type_len, CHAR8 *handler, UINTN handler_len){ EFI_FILE_HANDLE root,f; CHAR16 path[220]; UINT8 buf[1024]; UINTN sz=sizeof(buf)-1,i=0,j; EFI_STATUS s; type[0]=0; handler[0]=0; s=open_root(ih,st,&root); if(EFI_ERROR(s)) return; SPrint(path,sizeof(path),L"\\.meta\\%s.meta",name); s=uefi_call_wrapper(root->Open,5,root,&f,path,EFI_FILE_MODE_READ,0); if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return; } s=uefi_call_wrapper(f->Read,3,f,&sz,buf); uefi_call_wrapper(f->Close,1,f); uefi_call_wrapper(root->Close,1,root); if(EFI_ERROR(s)||sz==0) return; buf[sz]=0; while(i<sz){ j=i; while(j<sz && buf[j]!='\n' && buf[j]!='\r') j++; if(j>i){ if(j-i>6 && contains_ci((CHAR8*)&buf[i],(const CHAR8*)"TYPE:")){ UINTN k=0,p=i+5; while(p<j && (buf[p]==' '||buf[p]=='\t')) p++; while(p<j && k+1<type_len) type[k++]=buf[p++]; type[k]=0; } if(j-i>9 && contains_ci((CHAR8*)&buf[i],(const CHAR8*)"HANDLER:")){ UINTN k=0,p=i+8; while(p<j && (buf[p]==' '||buf[p]=='\t')) p++; while(p<j && k+1<handler_len) handler[k++]=buf[p++]; handler[k]=0; } } while(j<sz && (buf[j]=='\n'||buf[j]=='\r')) j++; i=j; } }

static BOOLEAN le16(UINT8 *p, UINT16 *v){ *v=(UINT16)(p[0]|(p[1]<<8)); return TRUE; }
static BOOLEAN le32(UINT8 *p, UINT32 *v){ *v=(UINT32)(p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24)); return TRUE; }
static EFI_STATUS read_file_alloc(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CONST CHAR16 *path, UINT8 **data_out, UINTN *size_out){
    EFI_FILE_HANDLE root,f; EFI_FILE_INFO *fi=NULL; UINTN info_sz=0,read_sz; EFI_STATUS s; UINT8 *buf=NULL;
    *data_out=NULL; *size_out=0;
    s=open_root(ih,st,&root); if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(root->Open,5,root,&f,(CHAR16*)path,EFI_FILE_MODE_READ,0); if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return s; }
    s=uefi_call_wrapper(f->GetInfo,4,f,&GenericFileInfo,&info_sz,NULL);
    if(s==EFI_BUFFER_TOO_SMALL){ s=uefi_call_wrapper(st->BootServices->AllocatePool,3,EfiLoaderData,info_sz,(VOID**)&fi); if(!EFI_ERROR(s)) s=uefi_call_wrapper(f->GetInfo,4,f,&GenericFileInfo,&info_sz,fi); }
    if(EFI_ERROR(s)||fi==NULL){ uefi_call_wrapper(f->Close,1,f); uefi_call_wrapper(root->Close,1,root); return EFI_LOAD_ERROR; }
    s=uefi_call_wrapper(st->BootServices->AllocatePool,3,EfiLoaderData,(UINTN)fi->FileSize,(VOID**)&buf);
    if(EFI_ERROR(s)||buf==NULL){ uefi_call_wrapper(st->BootServices->FreePool,1,fi); uefi_call_wrapper(f->Close,1,f); uefi_call_wrapper(root->Close,1,root); return EFI_OUT_OF_RESOURCES; }
    read_sz=(UINTN)fi->FileSize; s=uefi_call_wrapper(f->Read,3,f,&read_sz,buf);
    uefi_call_wrapper(st->BootServices->FreePool,1,fi); uefi_call_wrapper(f->Close,1,f); uefi_call_wrapper(root->Close,1,root);
    if(EFI_ERROR(s)||read_sz==0){ uefi_call_wrapper(st->BootServices->FreePool,1,buf); return EFI_LOAD_ERROR; }
    *data_out=buf; *size_out=read_sz; return EFI_SUCCESS;
}

static VOID load_tt_font(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, TT_FONT *tt){
    EFI_STATUS s; int off;
    tt->ok=FALSE; tt->data=NULL; tt->size=0; tt->scale=0.0f; tt->ascent=0; tt->descent=0; tt->line_gap=0;
    s=read_file_alloc(ih,st,L"\\.launcher\\DejaVuSans.ttf",&tt->data,&tt->size); if(EFI_ERROR(s)||tt->data==NULL) return;
    off=stbtt_GetFontOffsetForIndex(tt->data,0); if(off<0 || !stbtt_InitFont(&tt->font,tt->data,off)) return;
    tt->scale=stbtt_ScaleForPixelHeight(&tt->font,16.0f);
    stbtt_GetFontVMetrics(&tt->font,&tt->ascent,&tt->descent,&tt->line_gap);
    tt->ok=TRUE;
}


static VOID draw_tt_text(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, TT_FONT *tt, CHAR16 *text, int x, int y, EFI_GRAPHICS_OUTPUT_BLT_PIXEL fg, EFI_GRAPHICS_OUTPUT_BLT_PIXEL bgc){
    CHAR8 txt[LAUNCHER_NAME_MAX]; UINTN i=0,n=0; int baseline,pen_x,text_w=0,pad=2; UINTN bw,bh;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bg;
    if(tt==NULL || !tt->ok || text==NULL) return;
    while(text[i]!=0 && i+1<LAUNCHER_NAME_MAX){ CHAR16 ch=text[i]; txt[i]=(ch<128)?(CHAR8)ch:'?'; i++; } txt[i]=0; n=i;
    for(i=0;i<n;i++){
        int cp=(unsigned char)txt[i],adv=0,lsb=0;
        stbtt_GetCodepointHMetrics(&tt->font,cp,&adv,&lsb);
        text_w += (int)(adv*tt->scale);
        if(i+1<n) text_w += (int)(stbtt_GetCodepointKernAdvance(&tt->font,cp,(unsigned char)txt[i+1])*tt->scale);
        (void)lsb;
    }
    if(text_w<1) text_w=1;
    bw=(UINTN)(text_w+pad*2); bh=16;
    if(x<0||y<0) return;
    fill(rt,g,(UINTN)x,(UINTN)y,bw,bh,bgc);
    bg=AllocatePool(bw*bh*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)); if(bg==NULL) return;
    rt_read(rt,g,(UINTN)x,(UINTN)y,bw,bh,bg);
    baseline=y + (int)(tt->ascent * tt->scale);
    pen_x=x+pad;
    for(i=0;i<n;i++){
        int cp=(unsigned char)txt[i],gw,gh,xoff,yoff,adv=0,lsb=0; unsigned char *bm; UINTN row,col;
        bm=stbtt_GetCodepointBitmap(&tt->font,0,tt->scale,cp,&gw,&gh,&xoff,&yoff);
        if(bm!=NULL){
            int gx=pen_x+xoff, gy=baseline+yoff;
            for(row=0;row<(UINTN)gh;row++) for(col=0;col<(UINTN)gw;col++){
                UINTN dx=(UINTN)(gx+(int)col), dy=(UINTN)(gy+(int)row);
                if(dx>=(UINTN)x && dx<(UINTN)x+bw && dy>=(UINTN)y && dy<(UINTN)y+bh){
                    UINTN bi=(dy-(UINTN)y)*bw + (dx-(UINTN)x); UINT8 a=bm[row*(UINTN)gw+col];
                    if(a){ UINT8 inv=(UINT8)(255-a);
                        bg[bi].Red=(UINT8)((fg.Red*a + bg[bi].Red*inv)/255);
                        bg[bi].Green=(UINT8)((fg.Green*a + bg[bi].Green*inv)/255);
                        bg[bi].Blue=(UINT8)((fg.Blue*a + bg[bi].Blue*inv)/255);
                    }
                }
            }
            stbtt_FreeBitmap(bm,NULL);
        }
        stbtt_GetCodepointHMetrics(&tt->font,cp,&adv,&lsb); pen_x += (int)(adv*tt->scale);
        if(i+1<n) pen_x += (int)(stbtt_GetCodepointKernAdvance(&tt->font,cp,(unsigned char)txt[i+1])*tt->scale);
        (void)lsb;
    }
    rt_write(rt,g,(UINTN)x,(UINTN)y,bw,bh,bg);
    FreePool(bg);
}

static VOID draw_label_centered_under_icon(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, TT_FONT *tt, CHAR16 *name, UINTN icon_x, UINTN icon_y){
    CHAR8 txt[LAUNCHER_NAME_MAX]; UINTN i=0,n=0,max_chars=10; int x,y,baseline,pen_x; int text_w=0,pad=2;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bg; UINTN bw,bh;
    if(tt==NULL || !tt->ok) return;
    while(name[i]!=0 && i+1<LAUNCHER_NAME_MAX){ CHAR16 ch=name[i]; txt[i]=(ch<128)?(CHAR8)ch:'?'; i++; } txt[i]=0; n=i;
    if(n>max_chars){ txt[max_chars-3]='.'; txt[max_chars-2]='.'; txt[max_chars-1]='.'; txt[max_chars]=0; n=max_chars; }
    for(i=0;i<n;i++){
        int cp=(unsigned char)txt[i],adv=0,lsb=0;
        stbtt_GetCodepointHMetrics(&tt->font,cp,&adv,&lsb);
        text_w += (int)(adv*tt->scale);
        if(i+1<n) text_w += (int)(stbtt_GetCodepointKernAdvance(&tt->font,cp,(unsigned char)txt[i+1])*tt->scale);
        (void)lsb;
    }
    if(text_w<1) text_w=1;
    bw=(UINTN)(text_w + pad*2); bh=16;
    x=(int)(icon_x + ((ICON_PX > bw) ? ((ICON_PX - bw)/2) : 0));
    y=(int)(icon_y + ICON_PX + 4);
    if(x<0||y<0) return;
    bg=AllocatePool(bw*bh*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)); if(bg==NULL) return;
    rt_read(rt,g,(UINTN)x,(UINTN)y,bw,bh,bg);
    baseline=y + (int)(tt->ascent * tt->scale);
    pen_x=x+pad;
    for(i=0;i<n;i++){
        int cp=(unsigned char)txt[i],gw,gh,xoff,yoff,adv=0,lsb=0; unsigned char *bm; UINTN row,col;
        bm=stbtt_GetCodepointBitmap(&tt->font,0,tt->scale,cp,&gw,&gh,&xoff,&yoff);
        if(bm!=NULL){
            int gx=pen_x+xoff, gy=baseline+yoff;
            for(row=0;row<(UINTN)gh;row++) for(col=0;col<(UINTN)gw;col++){
                UINTN dx=(UINTN)(gx+(int)col), dy=(UINTN)(gy+(int)row);
                if(dx>=(UINTN)x && dx<(UINTN)x+bw && dy>=(UINTN)y && dy<(UINTN)y+bh){
                    UINTN bi=(dy-(UINTN)y)*bw + (dx-(UINTN)x); UINT8 a=bm[row*(UINTN)gw+col];
                    if(a){ UINT8 inv=(UINT8)(255-a);
                        bg[bi].Red=(UINT8)((bg[bi].Red*inv)/255); bg[bi].Green=(UINT8)((bg[bi].Green*inv)/255); bg[bi].Blue=(UINT8)((bg[bi].Blue*inv)/255);
                    }
                }
            }
            stbtt_FreeBitmap(bm,NULL);
        }
        stbtt_GetCodepointHMetrics(&tt->font,cp,&adv,&lsb); pen_x += (int)(adv*tt->scale);
        if(i+1<n) pen_x += (int)(stbtt_GetCodepointKernAdvance(&tt->font,cp,(unsigned char)txt[i+1])*tt->scale);
        (void)lsb;
    }
    rt_write(rt,g,(UINTN)x,(UINTN)y,bw,bh,bg);
    FreePool(bg);
}

static VOID blit_icon_alpha(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, ICON_IMAGE *icon, UINTN x, UINTN y, EFI_GRAPHICS_OUTPUT_BLT_PIXEL bg){
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL tmp[ICON_PX * ICON_PX];
    UINTN i, n;
    if (icon == NULL || !icon->ok || icon->px == NULL) return;
    n = icon->w * icon->h;
    if (n > ICON_PX * ICON_PX) n = ICON_PX * ICON_PX;
    for (i = 0; i < n; i++) {
        UINTN a = icon->px[i].Reserved;
        tmp[i].Red   = (UINT8)((icon->px[i].Red   * a + bg.Red   * (255 - a)) / 255);
        tmp[i].Green = (UINT8)((icon->px[i].Green * a + bg.Green * (255 - a)) / 255);
        tmp[i].Blue  = (UINT8)((icon->px[i].Blue  * a + bg.Blue  * (255 - a)) / 255);
        tmp[i].Reserved = 0;
    }
    rt_write(rt,g,x,y,icon->w,icon->h,tmp);
}
static VOID blit_icon_alpha_scaled(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, ICON_IMAGE *icon, UINTN x, UINTN y, UINTN w, UINTN h, EFI_GRAPHICS_OUTPUT_BLT_PIXEL bg){
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *tmp;
    UINTN row,col,n;
    if(icon==NULL || !icon->ok || icon->px==NULL || icon->w==0 || icon->h==0 || w==0 || h==0) return;
    n=w*h;
    tmp=AllocatePool(n*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
    if(tmp==NULL) return;
    for(row=0;row<h;row++) for(col=0;col<w;col++){
        UINTN sx=(col*icon->w)/w, sy=(row*icon->h)/h, si=sy*icon->w+sx, di=row*w+col;
        UINTN a=icon->px[si].Reserved;
        tmp[di].Red=(UINT8)((icon->px[si].Red*a + bg.Red*(255-a))/255);
        tmp[di].Green=(UINT8)((icon->px[si].Green*a + bg.Green*(255-a))/255);
        tmp[di].Blue=(UINT8)((icon->px[si].Blue*a + bg.Blue*(255-a))/255);
        tmp[di].Reserved=0;
    }
    rt_write(rt,g,x,y,w,h,tmp);
    FreePool(tmp);
}

static VOID draw_item_cell(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, TT_FONT *ttfont, ITEM *items, ICON_IMAGE *icons, UINTN idx, UINTN start, UINTN cols, UINTN wx, UINTN cell_w, UINTN cell_pad_x, UINTN content_y, UINTN cell_h, BOOLEAN selected, EFI_GRAPHICS_OUTPUT_BLT_PIXEL win, EFI_GRAPHICS_OUTPUT_BLT_PIXEL selc){
    UINTN vi=idx-start,row=vi/cols,col=vi%cols,x=wx+12+col*cell_w+cell_pad_x/2,y=content_y+row*cell_h;
    fill(rt,g,x-2,y-2,ICON_PX+4,ICON_PX+4,win);
    if(selected){ fill(rt,g,x-2,y-2,ICON_PX+4,2,selc); fill(rt,g,x-2,y+ICON_PX,ICON_PX+4,2,selc); fill(rt,g,x-2,y,2,ICON_PX,selc); fill(rt,g,x+ICON_PX,y,2,ICON_PX,selc); }
    if(icons[idx].ok){ blit_icon_alpha(rt,g,&icons[idx],x,y,win); }
    draw_label_centered_under_icon(rt,g, ttfont, items[idx].name, x, y);
}

static VOID load_icon(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *icon_name, ICON_IMAGE *out){
    EFI_FILE_HANDLE root,f; EFI_STATUS s; CHAR16 path[200]; UINTN info_sz=0,sz,off=0; UINT32 img_off=0; EFI_FILE_INFO *fi=NULL; UINT8 *buf=NULL; UINT16 reserved,type,count; UINT32 dib_size,w,h,bpp,comp,imgsz; UINTN entry; UINT32 best_off=0,best_size=0,best_w=0,best_h=0; BOOLEAN best_png=FALSE;
    out->ok=FALSE; out->px=NULL; out->w=0; out->h=0;
    s=open_root(ih,st,&root); if(EFI_ERROR(s)) return;
    SPrint(path,sizeof(path),L"\\.launcher\\%s",icon_name);
    s=uefi_call_wrapper(root->Open,5,root,&f,path,EFI_FILE_MODE_READ,0); if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return; }
    s=uefi_call_wrapper(f->GetInfo,4,f,&GenericFileInfo,&info_sz,NULL);
    if(s==EFI_BUFFER_TOO_SMALL){ s=uefi_call_wrapper(st->BootServices->AllocatePool,3,EfiLoaderData,info_sz,(VOID**)&fi); if(!EFI_ERROR(s)) s=uefi_call_wrapper(f->GetInfo,4,f,&GenericFileInfo,&info_sz,fi); }
    if(EFI_ERROR(s)||fi==NULL){ goto done; }
    sz=(UINTN)fi->FileSize; if(sz<40) goto done;
    s=uefi_call_wrapper(st->BootServices->AllocatePool,3,EfiLoaderData,sz,(VOID**)&buf); if(EFI_ERROR(s)||buf==NULL) goto done;
    s=uefi_call_wrapper(f->Read,3,f,&sz,buf); if(EFI_ERROR(s)||sz<40) goto done;
    le16(buf,&reserved); le16(buf+2,&type); le16(buf+4,&count); if(reserved!=0||type!=1||count<1) goto done;
    for(entry=0; entry<count; entry++){ UINTN eoff=6+entry*16; UINT32 esize,eimg; UINT32 ew,eh; BOOLEAN epng=FALSE; if(eoff+16>sz) break; ew=buf[eoff+0]; eh=buf[eoff+1]; if(ew==0) ew=256; if(eh==0) eh=256; le32(buf+eoff+8,&esize); le32(buf+eoff+12,&eimg); if(eimg>=sz||esize==0) continue; if(eimg+esize>sz) esize=(UINT32)(sz-eimg); epng=(esize>=8 && buf[eimg]==0x89 && buf[eimg+1]==0x50 && buf[eimg+2]==0x4E && buf[eimg+3]==0x47); if(best_size==0 || (epng && !best_png) || ((epng==best_png) && (ew*eh > best_w*best_h))){ best_off=eimg; best_size=esize; best_w=ew; best_h=eh; best_png=epng; } }
    if(best_size==0) goto done;
    img_off=best_off; imgsz=best_size; off=img_off;
    if ((sz - off) >= 8 &&
        buf[off + 0] == 0x89 && buf[off + 1] == 0x50 &&
        buf[off + 2] == 0x4E && buf[off + 3] == 0x47 &&
        buf[off + 4] == 0x0D && buf[off + 5] == 0x0A &&
        buf[off + 6] == 0x1A && buf[off + 7] == 0x0A) {
        int pw = 0, ph = 0, pc = 0;
        UINT8 *png = stbi_load_from_memory((const stbi_uc *)(buf + off), (int)imgsz, &pw, &ph, &pc, 4);
        if (png != NULL && pw > 0 && ph > 0 && pw <= 512 && ph <= 512) {
            s = uefi_call_wrapper(st->BootServices->AllocatePool,3,EfiLoaderData,ICON_PX*ICON_PX*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL),(VOID**)&out->px); if(EFI_ERROR(s)||out->px==NULL){ stbi_image_free(png); goto done; }
            { UINTN row,col; for(row=0;row<ICON_PX;row++){ for(col=0;col<ICON_PX;col++){ UINTN sx=(col*(UINTN)pw)/ICON_PX, sy=(row*(UINTN)ph)/ICON_PX; UINTN src=(sy*(UINTN)pw+sx)*4, dst=row*ICON_PX+col; out->px[dst].Red=png[src+0]; out->px[dst].Green=png[src+1]; out->px[dst].Blue=png[src+2]; out->px[dst].Reserved=png[src+3]; } } }
            stbi_image_free(png);
            out->w=ICON_PX; out->h=ICON_PX; out->ok=TRUE; goto done;
        }
        if (png != NULL) stbi_image_free(png);
    }
    le32(buf+off,&dib_size); if(dib_size<40||off+40>sz) goto done; le32(buf+off+4,&w); le32(buf+off+8,&h); h/=2; le16(buf+off+14,(UINT16*)&bpp); le32(buf+off+16,&comp); if(bpp!=32||comp!=0) goto done;
    if(w==0||h==0||w>256||h>256) goto done;
    s=uefi_call_wrapper(st->BootServices->AllocatePool,3,EfiLoaderData,ICON_PX*ICON_PX*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL),(VOID**)&out->px); if(EFI_ERROR(s)||out->px==NULL) goto done;
    { UINTN row,col,base=off+dib_size; for(row=0;row<ICON_PX;row++){ for(col=0;col<ICON_PX;col++){ UINTN sx=(col*w)/ICON_PX, sy=(row*h)/ICON_PX; UINTN src=base+((h-1-sy)*w+sx)*4; UINTN dst=row*ICON_PX+col; if(src+3>=sz) goto done; out->px[dst].Blue=buf[src+0]; out->px[dst].Green=buf[src+1]; out->px[dst].Red=buf[src+2]; out->px[dst].Reserved=buf[src+3]; } } }
    out->w=ICON_PX; out->h=ICON_PX; out->ok=TRUE;
 done:
    if(fi) uefi_call_wrapper(st->BootServices->FreePool,1,fi);
    if(buf) uefi_call_wrapper(st->BootServices->FreePool,1,buf);
    uefi_call_wrapper(f->Close,1,f); uefi_call_wrapper(root->Close,1,root);
}

 
static VOID load_pointer_png(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, POINTER_IMAGE *out){
    UINT8 *file_data=NULL,*png=NULL; UINTN file_sz=0; int pw=0,ph=0,pc=0; EFI_STATUS s; UINTN i,n;
    out->ok=FALSE; out->w=0; out->h=0; out->px=NULL;
    s=read_file_alloc(ih,st,L"\\.launcher\\pointer.png",&file_data,&file_sz);
    if(EFI_ERROR(s)||file_data==NULL||file_sz==0) return;
    png=stbi_load_from_memory((const stbi_uc*)file_data,(int)file_sz,&pw,&ph,&pc,4);
    uefi_call_wrapper(st->BootServices->FreePool,1,file_data);
    if(png==NULL||pw<=0||ph<=0||pw>128||ph>128){ if(png) stbi_image_free(png); return; }
    s=uefi_call_wrapper(st->BootServices->AllocatePool,3,EfiLoaderData,(UINTN)pw*(UINTN)ph*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL),(VOID**)&out->px);
    if(EFI_ERROR(s)||out->px==NULL){ stbi_image_free(png); return; }
    n=(UINTN)pw*(UINTN)ph;
    for(i=0;i<n;i++){ UINTN si=i*4; out->px[i].Red=png[si+0]; out->px[i].Green=png[si+1]; out->px[i].Blue=png[si+2]; out->px[i].Reserved=png[si+3]; }
    stbi_image_free(png);
    out->w=(UINTN)pw; out->h=(UINTN)ph; out->ok=TRUE;
}

static VOID blit_pointer_alpha(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, POINTER_IMAGE *ptr_img, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bg, UINTN x, UINTN y){
    UINTN row,col,w=ptr_img->w,h=ptr_img->h,n=w*h;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *tmp;
    tmp=AllocatePool(n*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
    if(tmp==NULL){ uefi_call_wrapper(g->Blt,10,g,ptr_img->px,EfiBltBufferToVideo,0,0,x,y,w,h,0); return; }
    CopyMem(tmp,bg,n*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
    for(row=0;row<h;row++) for(col=0;col<w;col++){
        UINTN i=row*w+col; UINT8 a=ptr_img->px[i].Reserved, inv=(UINT8)(255-a);
        tmp[i].Red=(UINT8)((ptr_img->px[i].Red*a + tmp[i].Red*inv)/255);
        tmp[i].Green=(UINT8)((ptr_img->px[i].Green*a + tmp[i].Green*inv)/255);
        tmp[i].Blue=(UINT8)((ptr_img->px[i].Blue*a + tmp[i].Blue*inv)/255);
        tmp[i].Reserved=0;
    }
    uefi_call_wrapper(g->Blt,10,g,tmp,EfiBltBufferToVideo,0,0,x,y,w,h,0);
    FreePool(tmp);
}

#define MAX_WINDOWS 8
#define MIN_W 240
#define MIN_H 180
#define RESIZE_GRAB_PX 16

typedef struct {
    BOOLEAN used;
    CHAR16 cwd[256];
    ITEM items[MAX_ITEMS];
    ICON_IMAGE icons[MAX_ITEMS];
    UINTN count,scroll,sel,last_sel,last_click_ms;
    UINTN x,y,w,h,content_y,content_h,cols,rows,visible_rows,max_scroll;
} LAUNCHER_WINDOW;
static BOOLEAN rects_intersect(UINTN ax,UINTN ay,UINTN aw,UINTN ah,UINTN bx,UINTN by,UINTN bw,UINTN bh){ return !(ax+aw<=bx || bx+bw<=ax || ay+ah<=by || by+bh<=ay); }
static VOID draw_window(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, TT_FONT *ttfont, LAUNCHER_WINDOW *w, ICON_IMAGE *close_img, BOOLEAN show_close, EFI_GRAPHICS_OUTPUT_BLT_PIXEL win, EFI_GRAPHICS_OUTPUT_BLT_PIXEL title, EFI_GRAPHICS_OUTPUT_BLT_PIXEL selc, EFI_GRAPHICS_OUTPUT_BLT_PIXEL btn, EFI_GRAPHICS_OUTPUT_BLT_PIXEL white, BOOLEAN rename_mode, CHAR16 *rename_buf, CHAR16 *toast){
    UINTN start=w->scroll*w->cols,end=start+w->visible_rows*w->cols,j; if(end>w->count) end=w->count;
    fill(rt,g,w->x,w->y,w->w,w->h,win); fill(rt,g,w->x,w->y,w->w,30,title); { CHAR16 ttl[80]; SPrint(ttl,sizeof(ttl),L"Launcher - %s",w->cwd); draw_tt_text(rt,g,ttfont,ttl,(int)(w->x+12),(int)(w->y+7),white,title);} if(show_close){ fill(rt,g,w->x+w->w-22,w->y+7,12,12,btn); if(close_img&&close_img->ok) blit_icon_alpha_scaled(rt,g,close_img,w->x+w->w-22,w->y+7,12,12,btn); else draw_tt_text(rt,g,ttfont,L"X",(int)(w->x+w->w-19),(int)(w->y+6),white,btn); }
    for(j=start;j<end;j++) draw_item_cell(rt,g,ttfont,w->items,w->icons,j,start,w->cols,w->x,90,26,w->content_y,83,(j==w->sel),win,selc);
    if(rename_mode && w->sel<w->count){ UINTN vi=w->sel-start; UINTN row=vi/w->cols,col=vi%w->cols; if(w->sel>=start && w->sel<end){ UINTN x=w->x+12+col*90+26/2, y=w->content_y+row*83+ICON_PX+4; EFI_GRAPHICS_OUTPUT_BLT_PIXEL box={255,255,255,0},txt={0,0,0,0}; CHAR16 rb[LAUNCHER_NAME_MAX+2]; UINTN len=StrLen(rename_buf); StrCpy(rb,rename_buf); if(len+1<LAUNCHER_NAME_MAX+2){ rb[len]=L'|'; rb[len+1]=0; } fill(rt,g,x-1,y-1,ICON_PX+2,18,box); draw_tt_text(rt,g,ttfont,rb,(int)x,(int)y,txt,box);} }
    { EFI_GRAPHICS_OUTPUT_BLT_PIXEL grip={96,96,96,0}; fill(rt,g,w->x+w->w-12,w->y+w->h-4,8,1,grip); fill(rt,g,w->x+w->w-9,w->y+w->h-7,5,1,grip); fill(rt,g,w->x+w->w-6,w->y+w->h-10,2,1,grip); }
    if(w->max_scroll>0){ UINTN barx=w->x+w->w-14,bh=w->content_h-4,th=(bh*w->visible_rows)/w->rows,ty=w->content_y+2+((bh-th)*w->scroll)/w->max_scroll; EFI_GRAPHICS_OUTPUT_BLT_PIXEL sb={120,120,120,0},thumb={50,50,50,0}; fill(rt,g,barx,w->content_y+2,10,bh,sb); fill(rt,g,barx,ty,10,th,thumb);}
}

static VOID draw_message_bar(RENDER_TARGET *rt, EFI_GRAPHICS_OUTPUT_PROTOCOL *g, TT_FONT *ttfont, EFI_GRAPHICS_OUTPUT_BLT_PIXEL title, EFI_GRAPHICS_OUTPUT_BLT_PIXEL white, CHAR16 *msg){ UINTN bar_h=30, y=(rt->h>bar_h)?(rt->h-bar_h):0; fill(rt,g,0,y,rt->w,bar_h,title); if(msg&&msg[0]) draw_tt_text(rt,g,ttfont,msg,12,(int)(y+7),white,title); }

static VOID free_window_icons(EFI_SYSTEM_TABLE *st, LAUNCHER_WINDOW *w){ UINTN i; for(i=0;i<MAX_ITEMS;i++){ if(w->icons[i].px){ uefi_call_wrapper(st->BootServices->FreePool,1,w->icons[i].px); w->icons[i].px=NULL; } w->icons[i].ok=FALSE; w->icons[i].w=0; w->icons[i].h=0; } }
static VOID reload_window(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, LAUNCHER_WINDOW *w){ UINTN i,usable_w; free_window_icons(st,w); w->count=load_items(ih,st,w->items,w->cwd); for(i=0;i<w->count;i++) load_icon(ih,st,w->items[i].icon,&w->icons[i]); usable_w=(w->w>26)?(w->w-26):w->w; w->cols=usable_w/90; if(w->cols<1) w->cols=1; w->rows=(w->count+w->cols-1)/w->cols; w->visible_rows=w->content_h/83; if(w->visible_rows<1) w->visible_rows=1; w->max_scroll=(w->rows>w->visible_rows)?(w->rows-w->visible_rows):0; if(w->scroll>w->max_scroll) w->scroll=w->max_scroll; }
static VOID layout_window(LAUNCHER_WINDOW *w){ UINTN usable_w=(w->w>26)?(w->w-26):w->w; w->content_y=w->y+36; w->content_h=(w->h>46)?(w->h-46):1; w->cols=usable_w/90; if(w->cols<1) w->cols=1; w->rows=(w->count+w->cols-1)/w->cols; w->visible_rows=w->content_h/83; if(w->visible_rows<1) w->visible_rows=1; w->max_scroll=(w->rows>w->visible_rows)?(w->rows-w->visible_rows):0; if(w->scroll>w->max_scroll) w->scroll=w->max_scroll; }

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st){ EFI_STATUS s; RENDER_TARGET rt; EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL; EFI_GUID gg=gEfiGraphicsOutputProtocolGuid; EFI_SIMPLE_POINTER_PROTOCOL *sp=NULL; EFI_GUID spg=EFI_SIMPLE_POINTER_PROTOCOL_GUID; EFI_ABSOLUTE_POINTER_PROTOCOL *ap=NULL; EFI_GUID apg=EFI_ABSOLUTE_POINTER_PROTOCOL_GUID; EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *inex=NULL; EFI_GUID inexg=EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID; EFI_INPUT_KEY k; TT_FONT ttfont; EFI_EVENT events[4]; UINTN evn=0,which=0; POINTER_IMAGE ptr_img; ICON_IMAGE close_img; EFI_GRAPHICS_OUTPUT_BLT_PIXEL *ptr_bg=NULL; UINTN ptr_w=12,ptr_h=14; BOOLEAN ptr_bg_valid=FALSE; INTN px=200,py=140,ppx=-1,ppy=-1; BOOLEAN prev_left=FALSE,need_redraw=TRUE,rename_mode=FALSE; CHAR16 rename_buf[LAUNCHER_NAME_MAX]; CHAR16 toast[64]; UINTN toast_ticks=0;
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL desk={70,110,40,0},win={192,192,192,0},title={140,90,60,0},selc={255,220,40,0},ptr={0,0,255,0},btn={200,60,60,0},white={255,255,255,0};
 LAUNCHER_WINDOW wins[MAX_WINDOWS]; UINTN win_count=1; INTN active=0; UINTN i; BOOLEAN dragging=FALSE,resizing=FALSE; INTN drag_dx=0,drag_dy=0; UINTN resize_edge=0,ox=0,oy=0,ow=0,oh=0; BOOLEAN partial_redraw=FALSE; UINTN pr_old_x=0,pr_old_y=0,pr_old_w=0,pr_old_h=0,pr_new_x=0,pr_new_y=0,pr_new_w=0,pr_new_h=0;
 InitializeLib(ih,st); disable_uefi_watchdog(st); ZeroMem(&rt,sizeof(rt)); toast[0]=0; s=uefi_call_wrapper(st->BootServices->LocateProtocol,3,&gg,NULL,(VOID**)&g); if(EFI_ERROR(s)) return s; rt.w=g->Mode->Info->HorizontalResolution; rt.h=g->Mode->Info->VerticalResolution; rt.buf=AllocatePool(rt.w*rt.h*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)); rt.use_backbuf=(rt.buf!=NULL)?TRUE:FALSE; uefi_call_wrapper(st->BootServices->LocateProtocol,3,&spg,NULL,(VOID**)&sp); uefi_call_wrapper(st->BootServices->LocateProtocol,3,&apg,NULL,(VOID**)&ap); uefi_call_wrapper(st->BootServices->LocateProtocol,3,&inexg,NULL,(VOID**)&inex);
 load_tt_font(ih,st,&ttfont); load_pointer_png(ih,st,&ptr_img); if(ptr_img.ok){ ptr_w=ptr_img.w; ptr_h=ptr_img.h; } ptr_bg=AllocatePool(ptr_w*ptr_h*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)); if(ptr_bg==NULL){ ptr_w=12; ptr_h=14; }
 ZeroMem(wins,sizeof(wins)); wins[0].used=TRUE; StrCpy(wins[0].cwd,L"\\"); wins[0].w=(g->Mode->Info->HorizontalResolution*3)/4; wins[0].h=(g->Mode->Info->VerticalResolution*3)/4; wins[0].x=(g->Mode->Info->HorizontalResolution-wins[0].w)/2; wins[0].y=(g->Mode->Info->VerticalResolution-wins[0].h)/2; layout_window(&wins[0]); reload_window(ih,st,&wins[0]);
 if(inex && inex->WaitForKeyEx) events[evn++]=inex->WaitForKeyEx; else events[evn++]=st->ConIn->WaitForKey; if(sp&&sp->WaitForInput) events[evn++]=sp->WaitForInput; if(ap&&ap->WaitForInput) events[evn++]=ap->WaitForInput;
 close_img.ok=FALSE; close_img.w=0; close_img.h=0; close_img.px=NULL;
 { UINT8 *close_data=NULL,*close_png=NULL; UINTN close_sz=0; int cw=0,ch=0,cc=0; close_img.ok=FALSE; s=read_file_alloc(ih,st,L"\\.launcher\\close.png",&close_data,&close_sz); if(!EFI_ERROR(s)&&close_data&&close_sz){ close_png=stbi_load_from_memory((const stbi_uc*)close_data,(int)close_sz,&cw,&ch,&cc,4); uefi_call_wrapper(st->BootServices->FreePool,1,close_data); if(close_png&&cw>0&&ch>0&&cw<=256&&ch<=256){ UINTN i,n; s=uefi_call_wrapper(st->BootServices->AllocatePool,3,EfiLoaderData,(UINTN)cw*(UINTN)ch*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL),(VOID**)&close_img.px); if(!EFI_ERROR(s)&&close_img.px){ n=(UINTN)cw*(UINTN)ch; for(i=0;i<n;i++){ UINTN si=i*4; close_img.px[i].Red=close_png[si+0]; close_img.px[i].Green=close_png[si+1]; close_img.px[i].Blue=close_png[si+2]; close_img.px[i].Reserved=close_png[si+3]; } close_img.w=(UINTN)cw; close_img.h=(UINTN)ch; close_img.ok=TRUE; } } if(close_png) stbi_image_free(close_png); } }
 while(1){ if(ptr_bg_valid && ppx>=0 && ppy>=0){ uefi_call_wrapper(g->Blt,10,g,ptr_bg,EfiBltBufferToVideo,0,0,(UINTN)ppx,(UINTN)ppy,ptr_w,ptr_h,0); ptr_bg_valid=FALSE; } if(toast_ticks>0){ toast_ticks--; if(toast_ticks==0) toast[0]=0; } if(need_redraw){ UINTN widx; fill(&rt,g,0,0,g->Mode->Info->HorizontalResolution,g->Mode->Info->VerticalResolution,desk); for(widx=0;widx<win_count;widx++) draw_window(&rt,g,&ttfont,&wins[widx],&close_img,(win_count>1)?TRUE:FALSE,win,title,selc,btn,white,(rename_mode && (INTN)widx==active),rename_buf,toast); draw_message_bar(&rt,g,&ttfont,title,white,toast); need_redraw=FALSE; partial_redraw=FALSE; ptr_bg_valid=FALSE; if(rt.use_backbuf) uefi_call_wrapper(g->Blt,10,g,rt.buf,EfiBltBufferToVideo,0,0,0,0,rt.w,rt.h,0); }
 else if(partial_redraw){ UINTN widx,ix=0,iy=0,iw=0,ih=0; BOOLEAN overlap=FALSE; if(rects_intersect(pr_old_x,pr_old_y,pr_old_w,pr_old_h,pr_new_x,pr_new_y,pr_new_w,pr_new_h)){ UINTN ox2=pr_old_x+pr_old_w, oy2=pr_old_y+pr_old_h, nx2=pr_new_x+pr_new_w, ny2=pr_new_y+pr_new_h; ix=(pr_old_x>pr_new_x)?pr_old_x:pr_new_x; iy=(pr_old_y>pr_new_y)?pr_old_y:pr_new_y; { UINTN ex=(ox2<nx2)?ox2:nx2, ey=(oy2<ny2)?oy2:ny2; iw=(ex>ix)?(ex-ix):0; ih=(ey>iy)?(ey-iy):0; overlap=(iw>0&&ih>0); } } if(overlap){ if(iy>pr_old_y) fill(&rt,g,pr_old_x,pr_old_y,pr_old_w,iy-pr_old_y,desk); if(pr_old_y+pr_old_h>iy+ih) fill(&rt,g,pr_old_x,iy+ih,pr_old_w,(pr_old_y+pr_old_h)-(iy+ih),desk); if(ix>pr_old_x) fill(&rt,g,pr_old_x,iy,ix-pr_old_x,ih,desk); if(pr_old_x+pr_old_w>ix+iw) fill(&rt,g,ix+iw,iy,(pr_old_x+pr_old_w)-(ix+iw),ih,desk); } else fill(&rt,g,pr_old_x,pr_old_y,pr_old_w,pr_old_h,desk); for(widx=0;widx<win_count;widx++){ LAUNCHER_WINDOW *w=&wins[widx]; if(rects_intersect(pr_old_x,pr_old_y,pr_old_w,pr_old_h,w->x,w->y,w->w,w->h) || rects_intersect(pr_new_x,pr_new_y,pr_new_w,pr_new_h,w->x,w->y,w->w,w->h)) draw_window(&rt,g,&ttfont,w,&close_img,(win_count>1)?TRUE:FALSE,win,title,selc,btn,white,(rename_mode && (INTN)widx==active),rename_buf,toast);} draw_message_bar(&rt,g,&ttfont,title,white,toast); partial_redraw=FALSE; ptr_bg_valid=FALSE; if(rt.use_backbuf) uefi_call_wrapper(g->Blt,10,g,rt.buf,EfiBltBufferToVideo,0,0,0,0,rt.w,rt.h,0); }
 if(px>=0&&py>=0&&(UINTN)px+ptr_w<g->Mode->Info->HorizontalResolution&&(UINTN)py+ptr_h<g->Mode->Info->VerticalResolution){ if(ptr_bg){ uefi_call_wrapper(g->Blt,10,g,ptr_bg,EfiBltVideoToBltBuffer,(UINTN)px,(UINTN)py,0,0,ptr_w,ptr_h,0); ptr_bg_valid=TRUE; } }
 if(ptr_img.ok&&ptr_bg) blit_pointer_alpha(g,&ptr_img,ptr_bg,(UINTN)px,(UINTN)py); else { fill(&rt,g,px,py,2,14,ptr); fill(&rt,g,px,py,10,2,ptr);} ppx=px; ppy=py;
 s=uefi_call_wrapper(st->BootServices->WaitForEvent,3,evn,events,&which); if(EFI_ERROR(s)) break; if(which==0){ if(inex){ EFI_KEY_DATA kd; if(!EFI_ERROR(uefi_call_wrapper(inex->ReadKeyStrokeEx,2,inex,&kd))){ UINT32 sh=kd.KeyState.KeyShiftState; if(kd.Key.ScanCode==SCAN_ESC && (sh&EFI_SHIFT_STATE_VALID) && (sh&(EFI_RIGHT_CONTROL_PRESSED|EFI_LEFT_CONTROL_PRESSED)) && (sh&(EFI_RIGHT_ALT_PRESSED|EFI_LEFT_ALT_PRESSED))) break; if(kd.Key.ScanCode==SCAN_F12 || kd.Key.ScanCode==SCAN_PRINT){ if(!EFI_ERROR(save_screenshot_bmp(ih,st,g))){ UINTN rw; StrCpy(toast,L"Saved \\screenshot"); for(rw=0;rw<win_count;rw++) reload_window(ih,st,&wins[rw]); } else StrCpy(toast,L"Screenshot failed"); toast_ticks=120; need_redraw=TRUE; } else if(rename_mode && active>=0){ LAUNCHER_WINDOW *aw=&wins[active]; UINTN len=StrLen(rename_buf); if(kd.Key.ScanCode==SCAN_ESC){ rename_mode=FALSE; StrCpy(toast,L"Rename canceled"); toast_ticks=120; need_redraw=TRUE; } else if(kd.Key.UnicodeChar==CHAR_BACKSPACE){ if(len>0) rename_buf[len-1]=0; need_redraw=TRUE; } else if(kd.Key.UnicodeChar==CHAR_CARRIAGE_RETURN){ if(aw->sel<aw->count && !EFI_ERROR(rename_selected_with_meta(ih,st,aw->cwd,aw->items[aw->sel].name,rename_buf))){ reload_window(ih,st,aw); StrCpy(toast,L"Rename success"); } else StrCpy(toast,L"Rename failed"); toast_ticks=120; rename_mode=FALSE; need_redraw=TRUE; } else if(kd.Key.UnicodeChar>=32 && kd.Key.UnicodeChar<127 && len+1<LAUNCHER_NAME_MAX){ rename_buf[len]=kd.Key.UnicodeChar; rename_buf[len+1]=0; need_redraw=TRUE; } } else if(kd.Key.ScanCode==SCAN_DELETE && active>=0){ LAUNCHER_WINDOW *aw=&wins[active]; if(aw->sel<aw->count && StrCmp(aw->items[aw->sel].name,L"..")!=0){ if(!EFI_ERROR(delete_selected_with_meta(ih,st,aw->cwd,aw->items[aw->sel].name))){ reload_window(ih,st,aw); if(aw->count==0) aw->sel=0; else if(aw->sel>=aw->count) aw->sel=aw->count-1; StrCpy(toast,L"Delete success"); toast_ticks=120; need_redraw=TRUE; } else { StrCpy(toast,L"Delete failed"); toast_ticks=120; need_redraw=TRUE; } } } } } else if(!EFI_ERROR(uefi_call_wrapper(st->ConIn->ReadKeyStroke,2,st->ConIn,&k))){ if(k.ScanCode==SCAN_F12 || k.ScanCode==SCAN_PRINT){ if(!EFI_ERROR(save_screenshot_bmp(ih,st,g))){ UINTN rw; StrCpy(toast,L"Saved \\screenshot"); for(rw=0;rw<win_count;rw++) reload_window(ih,st,&wins[rw]); } else StrCpy(toast,L"Screenshot failed"); toast_ticks=120; need_redraw=TRUE; } else if(k.ScanCode==SCAN_DELETE && active>=0){ LAUNCHER_WINDOW *aw=&wins[active]; if(aw->sel<aw->count && StrCmp(aw->items[aw->sel].name,L"..")!=0){ if(!EFI_ERROR(delete_selected_with_meta(ih,st,aw->cwd,aw->items[aw->sel].name))){ reload_window(ih,st,aw); if(aw->count==0) aw->sel=0; else if(aw->sel>=aw->count) aw->sel=aw->count-1; StrCpy(toast,L"Delete success"); toast_ticks=120; need_redraw=TRUE; } else { StrCpy(toast,L"Delete failed"); toast_ticks=120; need_redraw=TRUE; } } } } }
 { EFI_SIMPLE_POINTER_STATE ps; ZeroMem(&ps,sizeof(ps)); if(ap){ EFI_ABSOLUTE_POINTER_STATE as; if(!EFI_ERROR(uefi_call_wrapper(ap->GetState,2,ap,&as))){ UINT64 minx=ap->Mode->AbsoluteMinX,maxx=ap->Mode->AbsoluteMaxX,miny=ap->Mode->AbsoluteMinY,maxy=ap->Mode->AbsoluteMaxY,rx=(maxx>minx)?(maxx-minx):1,ry=(maxy>miny)?(maxy-miny):1; UINT64 ox2=(as.CurrentX>minx)?(as.CurrentX-minx):0,oy2=(as.CurrentY>miny)?(as.CurrentY-miny):0; px=(INTN)((ox2*(g->Mode->Info->HorizontalResolution-1))/rx); py=(INTN)((oy2*(g->Mode->Info->VerticalResolution-1))/ry); if(as.ActiveButtons&EFI_ABSP_TouchActive) ps.LeftButton=1; } }
 if(sp && !EFI_ERROR(uefi_call_wrapper(sp->GetState,2,sp,&ps))){ INTN scale_x=1024,scale_y=1024,step_x,step_y; if(sp->Mode!=NULL){ if(sp->Mode->ResolutionX>0) scale_x=(INTN)(sp->Mode->ResolutionX/64); if(sp->Mode->ResolutionY>0) scale_y=(INTN)(sp->Mode->ResolutionY/64);} if(scale_x<1)scale_x=1; if(scale_y<1)scale_y=1; step_x=(INTN)(ps.RelativeMovementX/scale_x); step_y=(INTN)(ps.RelativeMovementY/scale_y); if(step_x==0 && ps.RelativeMovementX!=0) step_x=(ps.RelativeMovementX>0)?1:-1; if(step_y==0 && ps.RelativeMovementY!=0) step_y=(ps.RelativeMovementY>0)?1:-1; px += step_x; py += step_y; if(px<0)px=0; if(py<0)py=0;
 if(ps.RelativeMovementZ!=0){ INTN wi; for(wi=(INTN)win_count-1;wi>=0;wi--){ LAUNCHER_WINDOW *w=&wins[wi]; if(px>=(INTN)w->x&&px<(INTN)(w->x+w->w)&&py>=(INTN)w->y&&py<(INTN)(w->y+w->h)){ UINTN old_scroll=w->scroll; if(ps.RelativeMovementZ>0){ if(w->scroll<w->max_scroll) w->scroll++; } else { if(w->scroll>0) w->scroll--; } if(w->scroll!=old_scroll){ pr_old_x=w->x; pr_old_y=w->y; pr_old_w=w->w; pr_old_h=w->h; pr_new_x=w->x; pr_new_y=w->y; pr_new_w=w->w; pr_new_h=w->h; partial_redraw=TRUE; } break; } } } }
 if(ps.LeftButton && !prev_left){ INTN wi; for(wi=(INTN)win_count-1;wi>=0;wi--){ LAUNCHER_WINDOW *w=&wins[wi]; if(px>=(INTN)w->x&&px<(INTN)(w->x+w->w)&&py>=(INTN)w->y&&py<(INTN)(w->y+w->h)){ UINTN tmp; if((UINTN)wi!=win_count-1){ LAUNCHER_WINDOW t=*w; for(tmp=(UINTN)wi;tmp+1<win_count;tmp++) wins[tmp]=wins[tmp+1]; wins[win_count-1]=t; wi=(INTN)win_count-1; } active=wi; w=&wins[wi]; if(win_count>1 && py<(INTN)(w->y+30) && px>=(INTN)(w->x+w->w-22) && px<(INTN)(w->x+w->w-10)){ free_window_icons(st,w); for(tmp=(UINTN)wi;tmp+1<win_count;tmp++) wins[tmp]=wins[tmp+1]; if(win_count>1) win_count--; if(active>=(INTN)win_count) active=(INTN)win_count-1; need_redraw=TRUE; break; }
 if(py<(INTN)(w->y+30)){ dragging=TRUE; drag_dx=px-(INTN)w->x; drag_dy=py-(INTN)w->y; break; }
 if(px>=(INTN)(w->x+w->w-RESIZE_GRAB_PX)||py>=(INTN)(w->y+w->h-RESIZE_GRAB_PX)){ resizing=TRUE; ox=w->x;oy=w->y;ow=w->w;oh=w->h; resize_edge=1; break; }
 { UINTN start=w->scroll*w->cols,end=start+w->visible_rows*w->cols,hit=(UINTN)-1,j; EFI_TIME t; UINTN ms=0; BOOLEAN have_ms=FALSE; if(end>w->count) end=w->count; for(j=start;j<end;j++){ UINTN vi=j-start,row=vi/w->cols,col=vi%w->cols,ix=w->x+12+col*90+26/2,iy=w->content_y+row*83; if(px>=(INTN)(ix-6)&&px<(INTN)(ix+ICON_PX+6)&&py>=(INTN)(iy-6)&&py<(INTN)(iy+ICON_PX+22)){ hit=j; break; } }
 if(hit!=(UINTN)-1){ w->sel=hit; if(!EFI_ERROR(uefi_call_wrapper(st->RuntimeServices->GetTime,2,&t,NULL))){ ms=(UINTN)((((UINTN)t.Minute*60u)+(UINTN)t.Second)*1000u) + (UINTN)(t.Nanosecond/1000000u); have_ms=TRUE; }
 if(w->last_sel==hit && have_ms && (ms-w->last_click_ms<450)){ UINTN vi=hit-start,row=vi/w->cols,col=vi%w->cols,ix=w->x+12+col*90+26/2,iy=w->content_y+row*83; UINTN lx=ix,ly=iy+ICON_PX+4,lw=ICON_PX,lh=16; if(px>=(INTN)lx&&px<(INTN)(lx+lw)&&py>=(INTN)ly&&py<(INTN)(ly+lh) && StrCmp(w->items[hit].name,L"..")!=0){ rename_mode=TRUE; StrCpy(rename_buf,w->items[hit].name); need_redraw=TRUE; } else if(w->items[hit].is_dir){ if(StrCmp(w->items[hit].name,L"..")!=0 && win_count<MAX_WINDOWS){ LAUNCHER_WINDOW *nw=&wins[win_count++]; ZeroMem(nw,sizeof(*nw)); nw->used=TRUE; join_path(nw->cwd,sizeof(nw->cwd),w->cwd,w->items[hit].name); nw->w=w->w; nw->h=w->h; nw->x=w->x+24; nw->y=w->y+24; if(nw->x+nw->w>g->Mode->Info->HorizontalResolution) nw->x=g->Mode->Info->HorizontalResolution-nw->w; if(nw->y+nw->h>g->Mode->Info->VerticalResolution) nw->y=g->Mode->Info->VerticalResolution-nw->h; layout_window(nw); reload_window(ih,st,nw); active=(INTN)win_count-1; }
 else if(StrCmp(w->items[hit].name,L"..")==0){ UINTN len=StrLen(w->cwd); if(len>1){ while(len>1&&w->cwd[len-1]!=L'\\') len--; if(len<=1) w->cwd[1]=0; else w->cwd[len-1]=0; reload_window(ih,st,w);} } }
 else { CHAR8 type[64],handler[128]; read_meta_type_handler(ih,st,w->items[hit].name,type,sizeof(type),handler,sizeof(handler)); if(is_program_type(type)||handler[0]==0) run_item(ih,st,w->cwd,w->items[hit].name); else { CHAR16 h16[128]; ascii_to_char16(handler,h16,128); run_item_with_args(ih,st,w->cwd,h16,w->items[hit].name);} reload_window(ih,st,w); }
 }
 if(have_ms) w->last_click_ms=ms;
 w->last_sel=hit; pr_old_x=w->x; pr_old_y=w->y; pr_old_w=w->w; pr_old_h=w->h; pr_new_x=w->x; pr_new_y=w->y; pr_new_w=w->w; pr_new_h=w->h; partial_redraw=TRUE; }
 }
 break; } } }
 if(!ps.LeftButton){ dragging=FALSE; resizing=FALSE; }
 if(ps.LeftButton && dragging && active>=0){ LAUNCHER_WINDOW *w=&wins[active]; UINTN ox0=w->x,oy0=w->y,ow0=w->w,oh0=w->h; INTN nx=px-drag_dx,ny=py-drag_dy; if(nx<0)nx=0; if(ny<0)ny=0; if((UINTN)nx+w->w>g->Mode->Info->HorizontalResolution) nx=(INTN)(g->Mode->Info->HorizontalResolution-w->w); if((UINTN)ny+w->h>g->Mode->Info->VerticalResolution) ny=(INTN)(g->Mode->Info->VerticalResolution-w->h); if((UINTN)nx!=w->x || (UINTN)ny!=w->y){ pr_old_x=ox0; pr_old_y=oy0; pr_old_w=ow0; pr_old_h=oh0; w->x=(UINTN)nx; w->y=(UINTN)ny; layout_window(w); pr_new_x=w->x; pr_new_y=w->y; pr_new_w=w->w; pr_new_h=w->h; partial_redraw=TRUE; } }
 if(ps.LeftButton && resizing && active>=0 && resize_edge){ LAUNCHER_WINDOW *w=&wins[active]; UINTN ox0=w->x,oy0=w->y,ow0=w->w,oh0=w->h; INTN nw=(INTN)ow + (px-(INTN)(ox+ow-1)); INTN nh=(INTN)oh + (py-(INTN)(oy+oh-1)); if(nw<(INTN)MIN_W) nw=MIN_W; if(nh<(INTN)MIN_H) nh=MIN_H; if((UINTN)w->x+(UINTN)nw>g->Mode->Info->HorizontalResolution) nw=(INTN)(g->Mode->Info->HorizontalResolution-w->x); if((UINTN)w->y+(UINTN)nh>g->Mode->Info->VerticalResolution) nh=(INTN)(g->Mode->Info->VerticalResolution-w->y); if((UINTN)nw!=w->w || (UINTN)nh!=w->h){ pr_old_x=ox0; pr_old_y=oy0; pr_old_w=ow0; pr_old_h=oh0; w->w=(UINTN)nw; w->h=(UINTN)nh; layout_window(w); pr_new_x=w->x; pr_new_y=w->y; pr_new_w=w->w; pr_new_h=w->h; partial_redraw=TRUE; } }
 prev_left=ps.LeftButton?TRUE:FALSE; }
 }
 for(i=0;i<win_count;i++) free_window_icons(st,&wins[i]);
 if(rt.buf) FreePool(rt.buf);
 if(ttfont.data) uefi_call_wrapper(st->BootServices->FreePool,1,ttfont.data);
 if(ptr_bg) FreePool(ptr_bg);
 if(ptr_img.px) FreePool(ptr_img.px);
 if(close_img.px) uefi_call_wrapper(st->BootServices->FreePool,1,close_img.px);
 return EFI_SUCCESS;
}

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

static VOID fill(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, UINTN x, UINTN y, UINTN w, UINTN h, EFI_GRAPHICS_OUTPUT_BLT_PIXEL c){ if(!w||!h) return; uefi_call_wrapper(g->Blt,10,g,&c,EfiBltVideoFill,0,0,x,y,w,h,0);} 
static BOOLEAN contains_ci(const CHAR8 *s, const CHAR8 *pat){ UINTN i,j; for(i=0;s&&s[i];i++){ for(j=0;pat[j]&&s[i+j];j++){ CHAR8 a=s[i+j],b=pat[j]; if(a>='a'&&a<='z')a-=32; if(b>='a'&&b<='z')b-=32; if(a!=b) break;} if(!pat[j]) return TRUE;} return FALSE; }

static EFI_STATUS open_root(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, EFI_FILE_HANDLE *root){ EFI_LOADED_IMAGE *li; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs; EFI_STATUS s; s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,ih,&LoadedImageProtocol,(VOID**)&li); if(EFI_ERROR(s)) return s; s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,li->DeviceHandle,&FileSystemProtocol,(VOID**)&fs); if(EFI_ERROR(s)) return s; return uefi_call_wrapper(fs->OpenVolume,2,fs,root);} 

static VOID pick_icon(EFI_FILE_HANDLE root, CHAR16 *name, BOOLEAN is_dir, CHAR16 *icon){ CHAR16 meta_path[220]; EFI_FILE_HANDLE mf; UINT8 buf[META_BUF]; UINTN sz=META_BUF-1; EFI_STATUS s; const CHAR8 *txt=(const CHAR8*)buf; if(is_dir){ StrCpy(icon,L"folder_single_128.ico"); return; } SPrint(meta_path,sizeof(meta_path),L"\\.meta\\%s.meta",name); s=uefi_call_wrapper(root->Open,5,root,&mf,meta_path,EFI_FILE_MODE_READ,0); if(EFI_ERROR(s)){ StrCpy(icon,L"file_single_128.ico"); return; } s=uefi_call_wrapper(mf->Read,3,mf,&sz,buf); uefi_call_wrapper(mf->Close,1,mf); if(EFI_ERROR(s)){ StrCpy(icon,L"file_single_128.ico"); return; } buf[sz]=0; if(contains_ci(txt,(const CHAR8 *)"TYPE: Program")||contains_ci(txt,(const CHAR8 *)"TYPE: PRGM")) StrCpy(icon,L"program_single_128.ico");
 else if(contains_ci(txt,(const CHAR8 *)"TYPE: bitmap")||contains_ci(txt,(const CHAR8 *)"TYPE: bmp")||contains_ci(txt,(const CHAR8 *)"TYPE: gif")||contains_ci(txt,(const CHAR8 *)"TYPE: jpeg")||contains_ci(txt,(const CHAR8 *)"TYPE: jpg")) StrCpy(icon,L"graphic_file_single_128.ico");
 else StrCpy(icon,L"file_single_128.ico"); }

static UINTN load_items(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, ITEM *it){ EFI_FILE_HANDLE root,dir; EFI_STATUS s; UINTN n=0,bsz; UINT8 entbuf[512]; s=open_root(ih,st,&root); if(EFI_ERROR(s)) return 0; s=uefi_call_wrapper(root->Open,5,root,&dir,L"\\",EFI_FILE_MODE_READ,0); if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return 0;} while(n<MAX_ITEMS){ EFI_FILE_INFO *fi=(EFI_FILE_INFO*)entbuf; bsz=sizeof(entbuf); s=uefi_call_wrapper(dir->Read,3,dir,&bsz,fi); if(EFI_ERROR(s)||bsz==0) break; if(StrCmp(fi->FileName,L".")==0||StrCmp(fi->FileName,L"..")==0||StrCmp(fi->FileName,L".meta")==0||StrCmp(fi->FileName,L"Launcher Icons")==0) continue; StrnCpy(it[n].name,fi->FileName,LAUNCHER_NAME_MAX-1); it[n].name[LAUNCHER_NAME_MAX-1]=0; it[n].is_dir=(fi->Attribute&EFI_FILE_DIRECTORY)?TRUE:FALSE; pick_icon(root,it[n].name,it[n].is_dir,it[n].icon); n++; } uefi_call_wrapper(dir->Close,1,dir); uefi_call_wrapper(root->Close,1,root); return n; }

static EFI_STATUS run_item(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *name){ EFI_LOADED_IMAGE *li; EFI_DEVICE_PATH *dp; EFI_HANDLE h=NULL; EFI_STATUS s; CHAR16 path[180]; s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,ih,&LoadedImageProtocol,(VOID**)&li); if(EFI_ERROR(s)) return s; SPrint(path,sizeof(path),L"\\%s",name); dp=FileDevicePath(li->DeviceHandle,path); if(dp==NULL) return EFI_NOT_FOUND; s=uefi_call_wrapper(st->BootServices->LoadImage,6,FALSE,ih,dp,NULL,0,&h); if(EFI_ERROR(s)) return s; return uefi_call_wrapper(st->BootServices->StartImage,3,h,NULL,NULL); }
static EFI_STATUS run_item_with_args(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *prog, CHAR16 *arg){ EFI_LOADED_IMAGE *li,*child=NULL; EFI_DEVICE_PATH *dp; EFI_HANDLE h=NULL; EFI_STATUS s; CHAR16 path[180]; s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,ih,&LoadedImageProtocol,(VOID**)&li); if(EFI_ERROR(s)) return s; SPrint(path,sizeof(path),L"\\%s",prog); dp=FileDevicePath(li->DeviceHandle,path); if(dp==NULL) return EFI_NOT_FOUND; s=uefi_call_wrapper(st->BootServices->LoadImage,6,FALSE,ih,dp,NULL,0,&h); if(EFI_ERROR(s)) return s; if(arg!=NULL && arg[0]!=0){ s=uefi_call_wrapper(st->BootServices->HandleProtocol,3,h,&LoadedImageProtocol,(VOID**)&child); if(!EFI_ERROR(s) && child!=NULL){ child->LoadOptions=(VOID*)arg; child->LoadOptionsSize=(UINT32)((StrLen(arg)+1)*sizeof(CHAR16)); } } return uefi_call_wrapper(st->BootServices->StartImage,3,h,NULL,NULL); }
static BOOLEAN is_program_type(const CHAR8 *t){ return t && (contains_ci(t,(const CHAR8*)"PROGRAM") || contains_ci(t,(const CHAR8*)"PRGM")); }
static VOID ascii_to_char16(const CHAR8 *in, CHAR16 *out, UINTN out_len){ UINTN i=0; if(out_len==0) return; while(in&&in[i]&&i+1<out_len){ out[i]=(CHAR16)(in[i]&0xFF); i++; } out[i]=0; }
static VOID read_meta_type_handler(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *name, CHAR8 *type, UINTN type_len, CHAR8 *handler, UINTN handler_len){ EFI_FILE_HANDLE root,f; CHAR16 path[220]; UINT8 buf[1024]; UINTN sz=sizeof(buf)-1,i=0,j; EFI_STATUS s; type[0]=0; handler[0]=0; s=open_root(ih,st,&root); if(EFI_ERROR(s)) return; SPrint(path,sizeof(path),L"\\.meta\\%s.meta",name); s=uefi_call_wrapper(root->Open,5,root,&f,path,EFI_FILE_MODE_READ,0); if(EFI_ERROR(s)){ uefi_call_wrapper(root->Close,1,root); return; } s=uefi_call_wrapper(f->Read,3,f,&sz,buf); uefi_call_wrapper(f->Close,1,f); uefi_call_wrapper(root->Close,1,root); if(EFI_ERROR(s)||sz==0) return; buf[sz]=0; while(i<sz){ j=i; while(j<sz && buf[j]!='\n' && buf[j]!='\r') j++; if(j>i){ if(j-i>6 && contains_ci((CHAR8*)&buf[i],(const CHAR8*)"TYPE:")){ UINTN k=0,p=i+5; while(p<j && (buf[p]==' '||buf[p]=='\t')) p++; while(p<j && k+1<type_len) type[k++]=buf[p++]; type[k]=0; } if(j-i>9 && contains_ci((CHAR8*)&buf[i],(const CHAR8*)"HANDLER:")){ UINTN k=0,p=i+8; while(p<j && (buf[p]==' '||buf[p]=='\t')) p++; while(p<j && k+1<handler_len) handler[k++]=buf[p++]; handler[k]=0; } } while(j<sz && (buf[j]=='\n'||buf[j]=='\r')) j++; i=j; } }

static BOOLEAN le16(UINT8 *p, UINT16 *v){ *v=(UINT16)(p[0]|(p[1]<<8)); return TRUE; }
static BOOLEAN le32(UINT8 *p, UINT32 *v){ *v=(UINT32)(p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24)); return TRUE; }
static VOID draw_label_fixed(EFI_SYSTEM_TABLE *st, CHAR16 *name, UINTN text_col, UINTN text_row, UINTN max_chars) {
    CHAR16 tmp[LAUNCHER_NAME_MAX];
    UINTN i = 0, n = 0;
    if (max_chars < 3) max_chars = 3;
    while (name[i] != 0 && i + 1 < LAUNCHER_NAME_MAX) { tmp[i] = name[i]; i++; }
    tmp[i] = 0; n = i;
    if (n > max_chars) {
        if (max_chars > 3) {
            tmp[max_chars - 3] = L'.';
            tmp[max_chars - 2] = L'.';
            tmp[max_chars - 1] = L'.';
            tmp[max_chars] = 0;
            n = max_chars;
        }
    }
    uefi_call_wrapper(st->ConOut->SetCursorPosition, 3, st->ConOut, text_col, text_row);
    uefi_call_wrapper(st->ConOut->OutputString, 2, st->ConOut, tmp);
}
static UINT8 glyph3x5(CHAR16 ch, UINTN row){
    if (ch >= L'a' && ch <= L'z') ch = (CHAR16)(ch - 32);
    switch (ch) {
        case L'A': { static const UINT8 g[5]={2,5,7,5,5}; return g[row]; }
        case L'B': { static const UINT8 g[5]={6,5,6,5,6}; return g[row]; }
        case L'C': { static const UINT8 g[5]={3,4,4,4,3}; return g[row]; }
        case L'D': { static const UINT8 g[5]={6,5,5,5,6}; return g[row]; }
        case L'E': { static const UINT8 g[5]={7,4,6,4,7}; return g[row]; }
        case L'F': { static const UINT8 g[5]={7,4,6,4,4}; return g[row]; }
        case L'G': { static const UINT8 g[5]={3,4,5,5,3}; return g[row]; }
        case L'H': { static const UINT8 g[5]={5,5,7,5,5}; return g[row]; }
        case L'I': { static const UINT8 g[5]={7,2,2,2,7}; return g[row]; }
        case L'J': { static const UINT8 g[5]={1,1,1,5,2}; return g[row]; }
        case L'K': { static const UINT8 g[5]={5,5,6,5,5}; return g[row]; }
        case L'L': { static const UINT8 g[5]={4,4,4,4,7}; return g[row]; }
        case L'M': { static const UINT8 g[5]={5,7,7,5,5}; return g[row]; }
        case L'N': { static const UINT8 g[5]={5,7,7,7,5}; return g[row]; }
        case L'O': { static const UINT8 g[5]={2,5,5,5,2}; return g[row]; }
        case L'P': { static const UINT8 g[5]={6,5,6,4,4}; return g[row]; }
        case L'Q': { static const UINT8 g[5]={2,5,5,3,1}; return g[row]; }
        case L'R': { static const UINT8 g[5]={6,5,6,5,5}; return g[row]; }
        case L'S': { static const UINT8 g[5]={3,4,2,1,6}; return g[row]; }
        case L'T': { static const UINT8 g[5]={7,2,2,2,2}; return g[row]; }
        case L'U': { static const UINT8 g[5]={5,5,5,5,7}; return g[row]; }
        case L'V': { static const UINT8 g[5]={5,5,5,5,2}; return g[row]; }
        case L'W': { static const UINT8 g[5]={5,5,7,7,5}; return g[row]; }
        case L'X': { static const UINT8 g[5]={5,5,2,5,5}; return g[row]; }
        case L'Y': { static const UINT8 g[5]={5,5,2,2,2}; return g[row]; }
        case L'Z': { static const UINT8 g[5]={7,1,2,4,7}; return g[row]; }
        case L'0': { static const UINT8 g[5]={7,5,5,5,7}; return g[row]; }
        case L'1': { static const UINT8 g[5]={2,6,2,2,7}; return g[row]; }
        case L'2': { static const UINT8 g[5]={6,1,7,4,7}; return g[row]; }
        case L'3': { static const UINT8 g[5]={6,1,7,1,6}; return g[row]; }
        case L'4': { static const UINT8 g[5]={5,5,7,1,1}; return g[row]; }
        case L'5': { static const UINT8 g[5]={7,4,7,1,6}; return g[row]; }
        case L'6': { static const UINT8 g[5]={3,4,7,5,2}; return g[row]; }
        case L'7': { static const UINT8 g[5]={7,1,1,2,2}; return g[row]; }
        case L'8': { static const UINT8 g[5]={2,5,2,5,2}; return g[row]; }
        case L'9': { static const UINT8 g[5]={2,5,7,1,6}; return g[row]; }
        case L'.': { static const UINT8 g[5]={0,0,0,0,2}; return g[row]; }
        case L'-': { static const UINT8 g[5]={0,0,7,0,0}; return g[row]; }
        case L'_': { static const UINT8 g[5]={0,0,0,0,7}; return g[row]; }
        default: return 0;
    }
}
static VOID draw_label_centered_under_icon(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, CHAR16 *name, UINTN icon_x, UINTN icon_y){
    CHAR16 tmp[LAUNCHER_NAME_MAX];
    UINTN i=0,n=0,max_chars=10,x,y,row,col;
    UINTN scale = 2;
    while(name[i]!=0 && i+1<LAUNCHER_NAME_MAX){ tmp[i]=name[i]; i++; }
    tmp[i]=0; n=i;
    if(n>max_chars){ tmp[max_chars-3]=L'.'; tmp[max_chars-2]=L'.'; tmp[max_chars-1]=L'.'; tmp[max_chars]=0; n=max_chars; }
    x = icon_x + ((ICON_PX > n*(3*scale + 1)) ? ((ICON_PX - n*(3*scale + 1))/2) : 0);
    y = icon_y + ICON_PX + 4;
    for(i=0;i<n;i++){
        for(row=0;row<5;row++){
            UINT8 bits = glyph3x5(tmp[i], row);
            for(col=0;col<3;col++){
                if(bits & (1U << (2-col))){
                    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black={0,0,0,0};
                    EFI_GRAPHICS_OUTPUT_BLT_PIXEL aa={80,80,80,0};
                    fill(g, x + i*(3*scale + 1) + col*scale, y + row*scale, scale, scale, black);
                    if (scale > 1) {
                        fill(g, x + i*(3*scale + 1) + col*scale + scale - 1, y + row*scale, 1, scale, aa);
                        fill(g, x + i*(3*scale + 1) + col*scale, y + row*scale + scale - 1, scale, 1, aa);
                    }
                }
            }
        }
    }
}
static VOID blit_icon_alpha(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, ICON_IMAGE *icon, UINTN x, UINTN y, EFI_GRAPHICS_OUTPUT_BLT_PIXEL bg){
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
    uefi_call_wrapper(g->Blt,10,g,tmp,EfiBltBufferToVideo,0,0,x,y,icon->w,icon->h,0);
}

static VOID load_icon(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, CHAR16 *icon_name, ICON_IMAGE *out){
    EFI_FILE_HANDLE root,f; EFI_STATUS s; CHAR16 path[200]; UINTN info_sz=0,sz,off=0; UINT32 img_off=0; EFI_FILE_INFO *fi=NULL; UINT8 *buf=NULL; UINT16 reserved,type,count; UINT32 dib_size,w,h,bpp,comp,imgsz; UINTN entry; UINT32 best_off=0,best_size=0,best_w=0,best_h=0; BOOLEAN best_png=FALSE;
    out->ok=FALSE; out->px=NULL; out->w=0; out->h=0;
    s=open_root(ih,st,&root); if(EFI_ERROR(s)) return;
    SPrint(path,sizeof(path),L"\\Launcher Icons\\%s",icon_name);
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

 EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st){ EFI_STATUS s; EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL; EFI_GUID gg=gEfiGraphicsOutputProtocolGuid; EFI_SIMPLE_POINTER_PROTOCOL *sp=NULL; EFI_GUID spg=EFI_SIMPLE_POINTER_PROTOCOL_GUID; EFI_ABSOLUTE_POINTER_PROTOCOL *ap=NULL; EFI_GUID apg=EFI_ABSOLUTE_POINTER_PROTOCOL_GUID; EFI_INPUT_KEY k; ITEM items[MAX_ITEMS]; ICON_IMAGE icons[MAX_ITEMS]; UINTN count,scroll=0,sel=(UINTN)-1,last_sel=(UINTN)-1,last_click_ms=0; UINTN cols=1,cell_w=90,cell_h=83,cell_pad_x=26; UINTN ww,wh,wx,wy,content_y,content_h,rows,visible_rows,max_scroll; BOOLEAN need_redraw=TRUE; INTN px=200,py=140,ppx=-1,ppy=-1; EFI_EVENT events[3]; UINTN evn=0,which=0; EFI_GRAPHICS_OUTPUT_BLT_PIXEL ptr_bg[12*14]; BOOLEAN ptr_bg_valid=FALSE; UINT64 last_abs_x=0,last_abs_y=0; UINT32 last_abs_buttons=0; BOOLEAN prev_left=FALSE;
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL desk={70,110,40,0},win={192,192,192,0},title={140,90,60,0},ic={190,190,80,0},selc={255,220,40,0},ptr={0,0,255,0};
 InitializeLib(ih,st); disable_uefi_watchdog(st);
 s=uefi_call_wrapper(st->BootServices->LocateProtocol,3,&gg,NULL,(VOID**)&g); if(EFI_ERROR(s)) return s;
 uefi_call_wrapper(st->BootServices->LocateProtocol,3,&spg,NULL,(VOID**)&sp);
 uefi_call_wrapper(st->BootServices->LocateProtocol,3,&apg,NULL,(VOID**)&ap);
 count=load_items(ih,st,items); { UINTN i; for(i=0;i<count;i++) load_icon(ih,st,items[i].icon,&icons[i]); }
 ww=(g->Mode->Info->HorizontalResolution*3)/4; wh=(g->Mode->Info->VerticalResolution*3)/4; wx=(g->Mode->Info->HorizontalResolution-ww)/2; wy=(g->Mode->Info->VerticalResolution-wh)/2; content_y=wy+36; content_h=wh-46; { UINTN usable_w=(ww>26)?(ww-26):ww; cols=usable_w/cell_w; if(cols<1) cols=1; } rows=(count+cols-1)/cols; visible_rows=content_h/cell_h; if(visible_rows<1) visible_rows=1; max_scroll=(rows>visible_rows)?(rows-visible_rows):0;
 events[evn++] = st->ConIn->WaitForKey;
 if (sp != NULL && sp->WaitForInput != NULL) events[evn++] = sp->WaitForInput;
 if (ap != NULL && ap->WaitForInput != NULL) events[evn++] = ap->WaitForInput;
 while(1){ UINTN i,start,end; EFI_SIMPLE_POINTER_STATE ps; ZeroMem(&ps, sizeof(ps));
  if(need_redraw){ start=scroll*cols; end=start+visible_rows*cols; if(end>count) end=count;
   fill(g,0,0,g->Mode->Info->HorizontalResolution,g->Mode->Info->VerticalResolution,desk); fill(g,wx,wy,ww,wh,win); fill(g,wx,wy,ww,30,title); PrintAt((wx/8)+2,wy/16,L"Launcher Desktop");
	   for(i=start;i<end;i++){ UINTN vi=i-start,row=vi/cols,col=vi%cols,x=wx+12+col*cell_w+cell_pad_x/2,y=content_y+row*cell_h; fill(g,x,y,ICON_PX,ICON_PX,win); if(i==sel){ fill(g,x-2,y-2,ICON_PX+4,2,selc); fill(g,x-2,y+ICON_PX,ICON_PX+4,2,selc); fill(g,x-2,y,2,ICON_PX,selc); fill(g,x+ICON_PX,y,2,ICON_PX,selc);} if(icons[i].ok){ blit_icon_alpha(g,&icons[i],x,y,win);} draw_label_centered_under_icon(g, items[i].name, x, y);} 
   if(max_scroll>0){ UINTN barx=wx+ww-14,bh=content_h-4,th=(bh*visible_rows)/rows,ty=content_y+2+((bh-th)*scroll)/max_scroll; EFI_GRAPHICS_OUTPUT_BLT_PIXEL sb={120,120,120,0},thumb={50,50,50,0}; fill(g,barx,content_y+2,10,bh,sb); fill(g,barx,ty,10,th,thumb);} 
   need_redraw=FALSE;
  }
	  if(ptr_bg_valid && ppx>=0 && ppy>=0){
	    uefi_call_wrapper(g->Blt,10,g,ptr_bg,EfiBltBufferToVideo,0,0,(UINTN)ppx,(UINTN)ppy,12,14,0);
	    ptr_bg_valid = FALSE;
	  }
	  if(px >= 0 && py >= 0 && (UINTN)px+12 < g->Mode->Info->HorizontalResolution && (UINTN)py+14 < g->Mode->Info->VerticalResolution){
	    uefi_call_wrapper(g->Blt,10,g,ptr_bg,EfiBltVideoToBltBuffer,(UINTN)px,(UINTN)py,0,0,12,14,0);
	    ptr_bg_valid = TRUE;
	  }
	  fill(g,px,py,2,14,ptr); fill(g,px,py,10,2,ptr); fill(g,px+2,py+2,8,2,ptr); fill(g,px+2,py+4,6,2,ptr); fill(g,px+2,py+6,4,2,ptr);
	  ppx=px; ppy=py;
  s = uefi_call_wrapper(st->BootServices->WaitForEvent,3,evn,events,&which);
  if(EFI_ERROR(s)) break;
  if(which==0 && !EFI_ERROR(uefi_call_wrapper(st->ConIn->ReadKeyStroke,2,st->ConIn,&k))) break;
	  if(ap != NULL){ EFI_ABSOLUTE_POINTER_STATE as; if(!EFI_ERROR(uefi_call_wrapper(ap->GetState,2,ap,&as))){ BOOLEAN changed = (as.CurrentX!=last_abs_x)||(as.CurrentY!=last_abs_y)||(as.ActiveButtons!=last_abs_buttons); if(changed){ UINT64 minx=ap->Mode->AbsoluteMinX,maxx=ap->Mode->AbsoluteMaxX,miny=ap->Mode->AbsoluteMinY,maxy=ap->Mode->AbsoluteMaxY; UINT64 rx=(maxx>minx)?(maxx-minx):1, ry=(maxy>miny)?(maxy-miny):1; UINT64 ox=(as.CurrentX>minx)?(as.CurrentX-minx):0, oy=(as.CurrentY>miny)?(as.CurrentY-miny):0; px=(INTN)((ox*(g->Mode->Info->HorizontalResolution-1))/rx); py=(INTN)((oy*(g->Mode->Info->VerticalResolution-1))/ry);} last_abs_x=as.CurrentX; last_abs_y=as.CurrentY; last_abs_buttons=as.ActiveButtons; if(as.ActiveButtons&EFI_ABSP_TouchActive) ps.LeftButton=1; }} 
	  if(sp && !EFI_ERROR(uefi_call_wrapper(sp->GetState,2,sp,&ps))){ INTN scale_x=1024,scale_y=1024,step_x,step_y; if(sp->Mode!=NULL){ if(sp->Mode->ResolutionX>0) scale_x=(INTN)(sp->Mode->ResolutionX/64); if(sp->Mode->ResolutionY>0) scale_y=(INTN)(sp->Mode->ResolutionY/64);} if(scale_x<1)scale_x=1; if(scale_y<1)scale_y=1; step_x=(INTN)(ps.RelativeMovementX/scale_x); step_y=(INTN)(ps.RelativeMovementY/scale_y); if(step_x==0 && ps.RelativeMovementX!=0) step_x=(ps.RelativeMovementX>0)?1:-1; if(step_y==0 && ps.RelativeMovementY!=0) step_y=(ps.RelativeMovementY>0)?1:-1; px += step_x; py += step_y; if(px<0)px=0; if(py<0)py=0; if((UINTN)px>=g->Mode->Info->HorizontalResolution)px=g->Mode->Info->HorizontalResolution-1; if((UINTN)py>=g->Mode->Info->VerticalResolution)py=g->Mode->Info->VerticalResolution-1;
    if(ps.RelativeMovementZ>0 && scroll>0){ scroll--; need_redraw=TRUE; } if(ps.RelativeMovementZ<0 && scroll<max_scroll){ scroll++; need_redraw=TRUE; }
	    if(ps.LeftButton && !prev_left){ UINTN start2=scroll*cols, end2=start2+visible_rows*cols, hit=(UINTN)-1, j; if(end2>count) end2=count; for(j=start2;j<end2;j++){ UINTN vi=j-start2,row=vi/cols,col=vi%cols,ix=wx+12+col*cell_w+cell_pad_x/2,iy=content_y+row*cell_h; if(px>=(INTN)(ix-6) && px<(INTN)(ix+ICON_PX+6) && py>=(INTN)(iy-6) && py<(INTN)(iy+ICON_PX+6)){ hit=j; break; } } if(hit!=(UINTN)-1){ if(sel!=hit){ sel=hit; need_redraw=TRUE; } { EFI_TIME t; if(last_sel==hit && !items[hit].is_dir && !EFI_ERROR(uefi_call_wrapper(st->RuntimeServices->GetTime,2,&t,NULL))){ UINTN ms=t.Second*1000+t.Nanosecond/1000000; if(ms-last_click_ms<450){ CHAR8 type[64],handler[128]; read_meta_type_handler(ih,st,items[hit].name,type,sizeof(type),handler,sizeof(handler)); if(is_program_type(type) || handler[0]==0){ run_item(ih,st,items[hit].name); } else { CHAR16 h16[128]; ascii_to_char16(handler,h16,128); run_item_with_args(ih,st,h16,items[hit].name); } count=load_items(ih,st,items); { UINTN usable_w=(ww>26)?(ww-26):ww; cols=usable_w/cell_w; if(cols<1) cols=1; rows=(count+cols-1)/cols; visible_rows=content_h/cell_h; if(visible_rows<1) visible_rows=1; max_scroll=(rows>visible_rows)?(rows-visible_rows):0; if(scroll>max_scroll) scroll=max_scroll; } need_redraw=TRUE; } last_click_ms=ms; } last_sel=hit; } } }
	    prev_left = ps.LeftButton ? TRUE : FALSE;
  }
 }
 { UINTN i; for(i=0;i<count;i++) if(icons[i].px) uefi_call_wrapper(st->BootServices->FreePool,1,icons[i].px); }
 return EFI_SUCCESS;
}


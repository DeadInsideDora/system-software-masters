#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "spo_vm.h"
#include <errno.h>
#include <limits.h>

#ifdef __clang__
#if __has_warning("-Wcompound-token-split-by-macro")
#pragma clang diagnostic ignored "-Wcompound-token-split-by-macro"
#endif
#endif

static SpoVM *handle(SV *self) {
    if(!SvROK(self) || !sv_derived_from(self,"SPO::MoarVM"))
        croak("expected a SPO::MoarVM object");
    SpoVM *vm=INT2PTR(SpoVM *,SvIV(SvRV(self)));
    if(!vm) croak("MoarVM handle is closed");
    return vm;
}
static int value_from_sv(SV *sv,char type,SpoValue *out,char *error,size_t size) {
    out->type=type;
    if(!SvOK(sv)) { snprintf(error,size,"undefined argument"); return -1; }
    if(type=='I' || type=='S') {
        if(!SvROK(sv) || SvTYPE(SvRV(sv))!=SVt_PVAV) {
            snprintf(error,size,"expected an array reference"); return -1;
        }
        AV *array=(AV *)SvRV(sv);
        out->length=(size_t)(av_len(array)+1);
        out->items=calloc(out->length?out->length:1,sizeof(*out->items));
        if(!out->items) { snprintf(error,size,"out of memory"); return -1; }
        for(size_t i=0;i<out->length;i++) {
            SV **item=av_fetch(array,(SSize_t)i,0);
            if(!item || value_from_sv(*item,type=='I'?'i':'s',&out->items[i],error,size)) {
                if(!item) snprintf(error,size,"sparse array element");
                return -1;
            }
        }
        return 0;
    }
    if(SvROK(sv)) { snprintf(error,size,"expected a scalar argument"); return -1; }
    STRLEN length; const char *text=SvPVutf8(sv,length);
    if(type=='s') {
        if(!is_utf8_string((const U8 *)text,length)) {
            snprintf(error,size,"invalid UTF-8 string"); return -1;
        }
        out->string=malloc(length+1);
        if(!out->string) { snprintf(error,size,"out of memory"); return -1; }
        memcpy(out->string,text,length); out->string[length]=0; out->length=length;
        return 0;
    }
    if(type=='i') {
        size_t pos=0;
        if(length && (text[0]=='-' || text[0]=='+')) pos=1;
        if(pos==length) { snprintf(error,size,"expected an int64 integer"); return -1; }
        for(size_t i=pos;i<length;i++) if(text[i]<'0' || text[i]>'9') {
            snprintf(error,size,"expected an int64 integer"); return -1;
        }
        errno=0; char *end; long long n=strtoll(text,&end,10);
        if(errno==ERANGE || (size_t)(end-text)!=length) {
            snprintf(error,size,"integer outside int64 range"); return -1;
        }
        out->integer=(int64_t)n; return 0;
    }
    snprintf(error,size,"unsupported argument type"); return -1;
}
static SV *value_to_sv(const SpoValue *value) {
    if(value->type=='v') return newSV(0);
    if(value->type=='i') return newSViv((IV)value->integer);
    if(value->type=='s') {
        SV *sv=newSVpvn(value->string,value->length); SvUTF8_on(sv); return sv;
    }
    AV *array=newAV();
    for(size_t i=0;i<value->length;i++) av_push(array,value_to_sv(&value->items[i]));
    return newRV_noinc((SV *)array);
}

MODULE = SPO::MoarVM PACKAGE = SPO::MoarVM
PROTOTYPES: DISABLE

SV *
new(class, path)
    const char *class
    SV *path
  PREINIT:
    char error[1024];
    STRLEN length;
    const char *filename;
    SpoVM *vm;
  CODE:
    if(!SvOK(path) || SvROK(path)) croak("expected a library filename");
    filename=SvPVutf8(path,length);
    if(!length || memchr(filename,0,length) || !is_utf8_string((const U8 *)filename,length))
        croak("invalid library filename");
    vm=spo_vm_open(filename,error,sizeof(error));
    if(!vm) croak("MoarVM load failed: %s",error);
    RETVAL=newSV(0);
    sv_setref_pv(RETVAL,class,(void *)vm);
  OUTPUT:
    RETVAL

SV *
exports(self)
    SV *self
  PREINIT:
    SpoVM *vm;
    HV *table;
  CODE:
    vm=handle(self); table=newHV();
    for(size_t i=0;i<spo_vm_export_count(vm);i++) {
        const char *name=spo_vm_export_name(vm,i);
        hv_store(table,name,(I32)strlen(name),newSVpv(spo_vm_signature(vm,name),0),0);
    }
    RETVAL=newRV_noinc((SV *)table);
  OUTPUT:
    RETVAL

SV *
call(self, name, ...)
    SV *self
    SV *name
  PREINIT:
    SpoVM *vm;
    const char *function_name;
    STRLEN name_length;
    const char *signature;
    SpoValue *args;
    SpoValue result={0};
    size_t count;
    int status=0;
    char error[1024]={0};
  CODE:
    vm=handle(self);
    if(!SvOK(name) || SvROK(name)) croak("expected a function name");
    function_name=SvPVutf8(name,name_length);
    if(!name_length || memchr(function_name,0,name_length)) croak("invalid function name");
    signature=spo_vm_signature(vm,function_name);
    if(!signature) croak("unknown exported function: %s",function_name);
    count=(size_t)(items-2);
    if(count!=strlen(signature)-3) croak("%s expects %zu arguments, got %zu",function_name,strlen(signature)-3,count);
    args=calloc(count?count:1,sizeof(*args));
    if(!args) croak("out of memory");
    for(size_t i=0;i<count && !status;i++)
        status=value_from_sv(ST(i+2),signature[i+2],&args[i],error,sizeof(error));
    if(!status) {
        status=spo_vm_call(vm,function_name,args,count,&result);
        if(status) snprintf(error,sizeof(error),"%s",spo_vm_error(vm));
    }
    for(size_t i=0;i<count;i++) spo_value_clear(&args[i]);
    free(args);
    if(status) { spo_value_clear(&result); croak("%s: %s",function_name,error); }
    RETVAL=value_to_sv(&result);
    spo_value_clear(&result);
  OUTPUT:
    RETVAL

void
close(self)
    SV *self
  CODE:
    if(!SvROK(self) || !sv_derived_from(self,"SPO::MoarVM")) croak("expected a SPO::MoarVM object");
    SpoVM *vm=INT2PTR(SpoVM *,SvIV(SvRV(self)));
    sv_setiv(SvRV(self),0);
    spo_vm_close(vm);

void
DESTROY(self)
    SV *self
  CODE:
    if(SvROK(self)) {
        SpoVM *vm=INT2PTR(SpoVM *,SvIV(SvRV(self)));
        sv_setiv(SvRV(self),0);
        spo_vm_close(vm);
    }

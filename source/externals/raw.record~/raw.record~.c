/**
	raw.record~ by antonioortegabrook
	Created May 21 2025
 */

#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"

typedef struct _raw_record {
	t_pxobject	ob;
	long		nchannels;
	t_filehandle	fh;
	long		rec_enabled;
	uint64_t	sample_count;
	uint64_t	byte_count;
	uint64_t	frame_count;
	uint64_t	buffer_size;	// Samples
	uint64_t	buffer_size_exponent;
	
	uint64_t	buffer_head;
	uint64_t	buffer_tail;
	double		*write_buffer;
	t_outlet	*notify_out;
} t_raw_record;

void *raw_record_new(long n);
void raw_record_free(t_raw_record *x);
void raw_record_assist(t_raw_record *x, void *b, long m, long a, char *s);
void raw_record_open(t_raw_record *x, t_symbol *s);
void raw_record_close(t_raw_record *x);
void raw_record_create(t_raw_record *x, t_symbol *s, long argc, t_atom *argv);
void raw_record_int(t_raw_record *x, long i);
void raw_record_dsp64(t_raw_record *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void raw_record_perform64(t_raw_record *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

static inline uint64_t next_pow2(uint64_t n);
static inline uint64_t pow2_exponent(uint64_t n);

static t_class *raw_record_class = NULL;

static t_symbol *S_NOTHING;

void ext_main(void *r)
{
	t_class *c = class_new("raw.record~", (method)raw_record_new, (method)raw_record_free, (long)sizeof(t_raw_record), 0L, A_DEFLONG, 0);

	class_addmethod(c,	(method)raw_record_open,	"open",		A_DEFSYM,	0);
	class_addmethod(c,	(method)raw_record_int,		"int",		A_FLOAT,	0);
	class_addmethod(c,	(method)raw_record_dsp64,	"dsp64",	A_CANT,		0);
	class_addmethod(c,	(method)raw_record_assist,	"assist",	A_CANT,		0);

	class_dspinit(c);
	class_register(CLASS_BOX, c);

	raw_record_class = c;
}

void* raw_record_new(long n)
//void* raw_record_new(t_symbol *s, long argc, t_atom *argv)
{
	t_raw_record *x = object_alloc(raw_record_class);

	// Agregar attr args process para saber cantidad de inlets
	if (x) {
		x->nchannels = !n ? 1 : labs(n);
		x->fh = NULL;
		x->buffer_size = next_pow2((8192 * x->nchannels)); // en el futuro, hacer variable?
		x->buffer_size_exponent = pow2_exponent(x->buffer_size);
		x->write_buffer = (double *)malloc(sizeof(double) * x->buffer_size);
		x->buffer_head = 0;
		x->buffer_tail = 0;
		
		dsp_setup((t_pxobject *)x, x->nchannels);	// MSP inlets: arg is # of inlets and is REQUIRED!
		x->notify_out = listout(x);			// Notifications outlet
	}
	
	S_NOTHING = gensym("");
	
	return (x);
}

void raw_record_free(t_raw_record *x)
{
	free(x->write_buffer);
	dsp_free((t_pxobject *)x);
}

void raw_record_assist(t_raw_record *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_INLET) { // inlet
		if (a == 0) {
			sprintf(s, "(signal) Ch 1, (int) Start/Stop recording)");
		} else {
			sprintf(s, "(signal) Ch %ld", a + 1);
		}
	}
	else { // outlet
		sprintf(s, "Notifications"); // # of Recorded frames?
	}
}

void raw_record_int(t_raw_record *x, long i)
{
	if (i) { // Start
		if (!x->fh) {
			object_error((t_object *)x, "Error: no file open");
			return;
		} // else
		x->rec_enabled = 1;
		x->buffer_head = 0;
		x->buffer_tail = 0;
		x->sample_count = 0;
		x->byte_count = 0;
	} else { // Stop
		if (x->rec_enabled) {
			// Write leftovers to file
			x->rec_enabled = 0;
			defer_low(x, (method)raw_record_close, NULL, 0, NULL);
		}
	}
}

void raw_record_open(t_raw_record *x, t_symbol *s)
{
	defer(x, (method)raw_record_create, s, 0, NULL);
}

void raw_record_close(t_raw_record *x)
{
	t_atom notify_list[3];
//	t_atom *notify_list = list;
	t_ptr_size leftover_size;
	t_ptr_size nbytes;
	if (x->buffer_head >= x->buffer_tail) {
		leftover_size = x->buffer_head - x->buffer_tail;
	} else {
		leftover_size = x->buffer_size - x->buffer_tail + x->buffer_head;
	}
	x->sample_count += leftover_size;
	nbytes = leftover_size * sizeof(double);

	sysfile_write(x->fh, &nbytes, x->write_buffer + x->buffer_tail);
	x->byte_count += nbytes;
	sysfile_seteof(x->fh, x->byte_count);
	sysfile_close(x->fh);
	x->fh = NULL;
	
	// Notify
	atom_setsym(notify_list, gensym("file"));
	atom_setsym(notify_list + 1, gensym("samples"));
	atom_setlong(notify_list + 2, x->sample_count);
	outlet_list(x->notify_out, 0L, 3, notify_list);
	
	atom_setsym(notify_list, gensym("file"));
	atom_setsym(notify_list + 1, gensym("bytes"));
	atom_setlong(notify_list + 2, x->byte_count);
	outlet_list(x->notify_out, 0L, 3, notify_list);
}

void raw_record_dowrite(t_raw_record *x, t_symbol *s, long argc, t_atom *argv)
{
	t_ptr_size nbytes = sizeof(double) * x->buffer_size >> 1;

	sysfile_write(x->fh, &nbytes, x->write_buffer + x->buffer_tail);
	x->byte_count += nbytes;
	x->buffer_tail += x->buffer_size >> 1;
	x->buffer_tail &= ((1 << x->buffer_size_exponent) - 1); // tail % buffer_size
}

void raw_record_create(t_raw_record *x, t_symbol *s, long argc, t_atom *argv)
{
	t_fourcc filetype = 'DATA', outtype;
	short numtypes = 1;
	char filename[MAX_FILENAME_CHARS];
	long err;
	short path;
	t_atom notify_list[3];
	
	if (x->fh) {
		sysfile_close(x->fh);
	}
	
	if (s == S_NOTHING) {
		strcpy(filename, "raw.data");
		if (saveasdialog_extended(filename, &path, &outtype, &filetype, numtypes)) {
			return;
		}
	} else {
		strcpy(filename, s->s_name);
		path = path_getdefault();
	}
	err = path_createsysfile(filename, path, filetype, &(x->fh));
	if (err) {
		object_error((t_object *)x, "Error %d creating file", err);
		return;
	}
	// Notify
	atom_setsym(notify_list, gensym("file"));
	atom_setsym(notify_list + 1, gensym("open"));
	atom_setsym(notify_list + 2, gensym(filename)); // Ver path
	outlet_list(x->notify_out, 0L, 3, notify_list);
}


void raw_record_dsp64(t_raw_record *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
{
//	long leftinletchannelcount = (long)object_method(dsp64,gensym("getnuminputchannels"), x, 0);
	object_method(dsp64, gensym("dsp_add64"), x, raw_record_perform64, 0, NULL);
}

void raw_record_perform64(t_raw_record *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
	uint64_t offset;
	if (x->rec_enabled) {
		t_atom argv;
		
		for (int i = 0; i < sampleframes; i++) {
			for (int j = 0; j < numins; j++) {
				*(x->write_buffer + x->buffer_head + i * numins + j) = ins[j][i]; // Interleave
			}
		}
		x->buffer_head += numins * sampleframes;
		
		if (x->buffer_head == x->buffer_size >> 1 || x->buffer_head == x->buffer_size) {
			// Do write!
			atom_setlong(&argv, x->buffer_head & ((1 << x->buffer_size_exponent) - 1)); // tail % buffer_size
			defer(x, (method)raw_record_dowrite, NULL, 1, &argv);
		}
		x->sample_count += numins * sampleframes;
		x->buffer_head = x->buffer_head & ((1 << x->buffer_size_exponent) - 1); // head % buffer_size
	}
}

// Util
static inline uint64_t next_pow2(uint64_t n)
{
	if (!n)
		return 0;
	n--;
	n |= 1;
	n |= 2;
	n |= 4;
	n |= 8;
	n |= 16;
	n |= 32;
	n++;
	
	return n;
}

static inline uint64_t pow2_exponent(uint64_t n)
{
	uint64_t k = 0;
	while (n >>= 1)
		k++;
	return k;
}

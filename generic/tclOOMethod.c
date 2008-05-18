/*
 * tclOOMethod.c --
 *
 *	This file contains code to create and manage methods.
 *
 * Copyright (c) 2005-2006 by Donal K. Fellows
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclOOMethod.c,v 1.19 2008/05/18 06:57:27 dkf Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tclInt.h"
#include "tclOOInt.h"

/*
 * Structure used to help delay computing names of objects or classes for
 * [info frame] until needed, making invokation faster in the normal case.
 */

struct PNI {
    Tcl_Interp *interp;		/* Interpreter in which to compute the name of
				 * a method. */
    Tcl_Method method;		/* Method to compute the name of. */
};

/*
 * Structure used to contain all the information needed about a call frame
 * used in a procedure-like method.
 */

typedef struct {
    CallFrame *framePtr;	/* Reference to the call frame itself (it's
				 * actually allocated on the Tcl stack). */
    ProcErrorProc errProc;	/* The error handler for the body. */
    Tcl_Obj *nameObj;		/* The "name" of the command. */
    Command cmd;		/* The command structure. Mostly bogus. */
    ExtraFrameInfo efi;		/* Extra information used for [info frame]. */
    struct PNI pni;		/* Specialist information used in the efi
				 * field for this type of call. */
} PMFrameData;

/*
 * Function declarations for things defined in this file.
 */

static Tcl_Obj **	InitEnsembleRewrite(Tcl_Interp *interp, int objc,
			    Tcl_Obj *const *objv, int toRewrite,
			    int rewriteLength, Tcl_Obj *const *rewriteObjs,
			    int *lengthPtr);
static int		InvokeProcedureMethod(ClientData clientData,
			    Tcl_Interp *interp, Tcl_ObjectContext context,
			    int objc, Tcl_Obj *const *objv);
static int		PushMethodCallFrame(Tcl_Interp *interp,
			    CallContext *contextPtr, ProcedureMethod *pmPtr,
			    int objc, Tcl_Obj *const *objv,
			    PMFrameData *fdPtr);
static void		DeleteProcedureMethodRecord(char *recordPtr);
static void		DeleteProcedureMethod(ClientData clientData);
static int		CloneProcedureMethod(Tcl_Interp *interp,
			    ClientData clientData, ClientData *newClientData);
static void		MethodErrorHandler(Tcl_Interp *interp,
			    Tcl_Obj *procNameObj);
static void		ConstructorErrorHandler(Tcl_Interp *interp,
			    Tcl_Obj *procNameObj);
static void		DestructorErrorHandler(Tcl_Interp *interp,
			    Tcl_Obj *procNameObj);
static Tcl_Obj *	RenderDeclarerName(ClientData clientData);
static int		InvokeForwardMethod(ClientData clientData,
			    Tcl_Interp *interp, Tcl_ObjectContext context,
			    int objc, Tcl_Obj *const *objv);
static void		DeleteForwardMethod(ClientData clientData);
static int		CloneForwardMethod(Tcl_Interp *interp,
			    ClientData clientData, ClientData *newClientData);

/*
 * The types of methods defined by the core OO system.
 */

static const Tcl_MethodType procMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT, "procedural method",
    InvokeProcedureMethod, DeleteProcedureMethod, CloneProcedureMethod
};
static const Tcl_MethodType fwdMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT, "forward",
    InvokeForwardMethod, DeleteForwardMethod, CloneForwardMethod
};

/*
 * ----------------------------------------------------------------------
 *
 * Tcl_NewInstanceMethod --
 *
 *	Attach a method to an object instance.
 *
 * ----------------------------------------------------------------------
 */

Tcl_Method
Tcl_NewInstanceMethod(
    Tcl_Interp *interp,		/* Unused? */
    Tcl_Object object,		/* The object that has the method attached to
				 * it. */
    Tcl_Obj *nameObj,		/* The name of the method. May be NULL; if so,
				 * up to caller to manage storage (e.g., when
				 * it is a constructor or destructor). */
    int flags,			/* Whether this is a public method. */
    const Tcl_MethodType *typePtr,
				/* The type of method this is, which defines
				 * how to invoke, delete and clone the
				 * method. */
    ClientData clientData)	/* Some data associated with the particular
				 * method to be created. */
{
    register Object *oPtr = (Object *) object;
    register Method *mPtr;
    Tcl_HashEntry *hPtr;
    int isNew;

    if (nameObj == NULL) {
	mPtr = (Method *) ckalloc(sizeof(Method));
	mPtr->namePtr = NULL;
	goto populate;
    }
    if (!oPtr->methodsPtr) {
	oPtr->methodsPtr = (Tcl_HashTable *) ckalloc(sizeof(Tcl_HashTable));
	Tcl_InitObjHashTable(oPtr->methodsPtr);
    }
    hPtr = Tcl_CreateHashEntry(oPtr->methodsPtr, (char *) nameObj, &isNew);
    if (isNew) {
	mPtr = (Method *) ckalloc(sizeof(Method));
	mPtr->namePtr = nameObj;
	Tcl_IncrRefCount(nameObj);
	Tcl_SetHashValue(hPtr, mPtr);
    } else {
	mPtr = Tcl_GetHashValue(hPtr);
	if (mPtr->typePtr != NULL && mPtr->typePtr->deleteProc != NULL) {
	    mPtr->typePtr->deleteProc(mPtr->clientData);
	}
    }

  populate:
    mPtr->typePtr = typePtr;
    mPtr->clientData = clientData;
    mPtr->flags = 0;
    mPtr->declaringObjectPtr = oPtr;
    mPtr->declaringClassPtr = NULL;
    if (flags) {
	mPtr->flags |= flags & (PUBLIC_METHOD | PRIVATE_METHOD);
    }
    oPtr->epoch++;
    return (Tcl_Method) mPtr;
}

/*
 * ----------------------------------------------------------------------
 *
 * Tcl_NewMethod --
 *
 *	Attach a method to a class.
 *
 * ----------------------------------------------------------------------
 */

Tcl_Method
Tcl_NewMethod(
    Tcl_Interp *interp,		/* The interpreter containing the class. */
    Tcl_Class cls,		/* The class to attach the method to. */
    Tcl_Obj *nameObj,		/* The name of the object. May be NULL (e.g.,
				 * for constructors or destructors); if so, up
				 * to caller to manage storage. */
    int flags,			/* Whether this is a public method. */
    const Tcl_MethodType *typePtr,
				/* The type of method this is, which defines
				 * how to invoke, delete and clone the
				 * method. */
    ClientData clientData)	/* Some data associated with the particular
				 * method to be created. */
{
    register Class *clsPtr = (Class *) cls;
    register Method *mPtr;
    Tcl_HashEntry *hPtr;
    int isNew;

    if (nameObj == NULL) {
	mPtr = (Method *) ckalloc(sizeof(Method));
	mPtr->namePtr = NULL;
	goto populate;
    }
    hPtr = Tcl_CreateHashEntry(&clsPtr->classMethods, (char *)nameObj,&isNew);
    if (isNew) {
	mPtr = (Method *) ckalloc(sizeof(Method));
	mPtr->namePtr = nameObj;
	Tcl_IncrRefCount(nameObj);
	Tcl_SetHashValue(hPtr, mPtr);
    } else {
	mPtr = Tcl_GetHashValue(hPtr);
	if (mPtr->typePtr != NULL && mPtr->typePtr->deleteProc != NULL) {
	    mPtr->typePtr->deleteProc(mPtr->clientData);
	}
    }

  populate:
    TclOOGetFoundation(interp)->epoch++;
    mPtr->typePtr = typePtr;
    mPtr->clientData = clientData;
    mPtr->flags = 0;
    mPtr->declaringObjectPtr = NULL;
    mPtr->declaringClassPtr = clsPtr;
    if (flags) {
	mPtr->flags |= flags & (PUBLIC_METHOD | PRIVATE_METHOD);
    }

    return (Tcl_Method) mPtr;
}

/*
 * ----------------------------------------------------------------------
 *
 * DeleteMethodStruct --
 *
 *	Function used when deleting a method. Always called indirectly via
 *	Tcl_EventuallyFree().
 *
 * ----------------------------------------------------------------------
 */

static void
DeleteMethodStruct(
    char *buffer)
{
    Method *mPtr = (Method *) buffer;

    if (mPtr->typePtr != NULL && mPtr->typePtr->deleteProc != NULL) {
	mPtr->typePtr->deleteProc(mPtr->clientData);
    }
    if (mPtr->namePtr != NULL) {
	Tcl_DecrRefCount(mPtr->namePtr);
    }

    ckfree(buffer);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOODeleteMethod --
 *
 *	How to delete a method.
 *
 * ----------------------------------------------------------------------
 */

void
TclOODeleteMethod(
    Method *mPtr)
{
    if (mPtr != NULL) {
	Tcl_EventuallyFree(mPtr, DeleteMethodStruct);
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewBasicMethod --
 *
 *	Helper that makes it cleaner to create very simple methods during
 *	basic system initialization. Not suitable for general use.
 *
 * ----------------------------------------------------------------------
 */

void
TclOONewBasicMethod(
    Tcl_Interp *interp,
    Class *clsPtr,		/* Class to attach the method to. */
    const DeclaredClassMethod *dcm)
				/* Name of the method, whether it is public,
				 * and the function to implement it. */
{
    Tcl_Obj *namePtr = Tcl_NewStringObj(dcm->name, -1);

    Tcl_IncrRefCount(namePtr);
    Tcl_NewMethod(interp, (Tcl_Class) clsPtr, namePtr,
	    (dcm->isPublic ? PUBLIC_METHOD : 0), &dcm->definition, NULL);
    Tcl_DecrRefCount(namePtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewProcInstanceMethod --
 *
 *	Create a new procedure-like method for an object.
 *
 * ----------------------------------------------------------------------
 */

Method *
TclOONewProcInstanceMethod(
    Tcl_Interp *interp,		/* The interpreter containing the object. */
    Object *oPtr,		/* The object to modify. */
    int flags,			/* Whether this is a public method. */
    Tcl_Obj *nameObj,		/* The name of the method, which must not be
				 * NULL. */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which must not be NULL. */
    Tcl_Obj *bodyObj,		/* The body of the method, which must not be
				 * NULL. */
    ProcedureMethod **pmPtrPtr)	/* Place to write pointer to procedure method
				 * structure to allow for deeper tuning of the
				 * structure's contents. NULL if caller is not
				 * interested. */
{
    int argsLen;
    register ProcedureMethod *pmPtr;
    Tcl_Method method;

    if (Tcl_ListObjLength(interp, argsObj, &argsLen) != TCL_OK) {
	return NULL;
    }
    pmPtr = (ProcedureMethod *) ckalloc(sizeof(ProcedureMethod));
    memset(pmPtr, 0, sizeof(ProcedureMethod));
    pmPtr->version = TCLOO_PROCEDURE_METHOD_VERSION;
    pmPtr->flags = flags & USE_DECLARER_NS;
    method = TclOOMakeProcInstanceMethod(interp, oPtr, flags, nameObj,
	    argsObj, bodyObj, &procMethodType, pmPtr, &pmPtr->procPtr);
    if (method == NULL) {
	ckfree((char *) pmPtr);
    } else if (pmPtrPtr != NULL) {
	*pmPtrPtr = pmPtr;
    }
    return (Method *) method;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewProcMethod --
 *
 *	Create a new procedure-like method for a class.
 *
 * ----------------------------------------------------------------------
 */

Method *
TclOONewProcMethod(
    Tcl_Interp *interp,		/* The interpreter containing the class. */
    Class *clsPtr,		/* The class to modify. */
    int flags,			/* Whether this is a public method. */
    Tcl_Obj *nameObj,		/* The name of the method, which may be NULL;
				 * if so, up to caller to manage storage
				 * (e.g., because it is a constructor or
				 * destructor). */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which may be NULL; if so, it is equivalent
				 * to an empty list. */
    Tcl_Obj *bodyObj,		/* The body of the method, which must not be
				 * NULL. */
    ProcedureMethod **pmPtrPtr)	/* Place to write pointer to procedure method
				 * structure to allow for deeper tuning of the
				 * structure's contents. NULL if caller is not
				 * interested. */
{
    int argsLen;		/* -1 => delete argsObj before exit */
    register ProcedureMethod *pmPtr;
    const char *procName;
    Tcl_Method method;

    if (argsObj == NULL) {
	argsLen = -1;
	argsObj = Tcl_NewObj();
	Tcl_IncrRefCount(argsObj);
	procName = "<destructor>";
    } else if (Tcl_ListObjLength(interp, argsObj, &argsLen) != TCL_OK) {
	return NULL;
    } else {
	procName = (nameObj==NULL ? "<constructor>" : TclGetString(nameObj));
    }

    pmPtr = (ProcedureMethod *) ckalloc(sizeof(ProcedureMethod));
    memset(pmPtr, 0, sizeof(ProcedureMethod));
    pmPtr->version = TCLOO_PROCEDURE_METHOD_VERSION;
    pmPtr->flags = flags & USE_DECLARER_NS;

    method = TclOOMakeProcMethod(interp, clsPtr, flags, nameObj,
	    procName, argsObj, bodyObj, &procMethodType, pmPtr,
	    &pmPtr->procPtr);

    if (argsLen == -1) {
	Tcl_DecrRefCount(argsObj);
    }
    if (method == NULL) {
	ckfree((char *) pmPtr);
    } else if (pmPtrPtr != NULL) {
	*pmPtrPtr = pmPtr;
    }

    return (Method *) method;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOMakeProcInstanceMethod --
 *
 *	The guts of the code to make a procedure-like method for an object.
 *	Split apart so that it is easier for other extensions to reuse (in
 *	particular, it frees them from having to pry so deeply into Tcl's
 *	guts).
 *
 * ----------------------------------------------------------------------
 */

Tcl_Method
TclOOMakeProcInstanceMethod(
    Tcl_Interp *interp,		/* The interpreter containing the object. */
    Object *oPtr,		/* The object to modify. */
    int flags,			/* Whether this is a public method. */
    Tcl_Obj *nameObj,		/* The name of the method, which _must not_ be
				 * NULL. */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which _must not_ be NULL. */
    Tcl_Obj *bodyObj,		/* The body of the method, which _must not_ be
				 * NULL. */
    const Tcl_MethodType *typePtr,
				/* The type of the method to create. */
    ClientData clientData,	/* The per-method type-specific data. */
    Proc **procPtrPtr)		/* A pointer to the variable in which to write
				 * the procedure record reference. Presumably
				 * inside the structure indicated by the
				 * pointer in clientData. */
{
    Interp *iPtr = (Interp *) interp;
    Proc *procPtr;

    if (TclCreateProc(interp, NULL, TclGetString(nameObj), argsObj, bodyObj,
	    procPtrPtr) != TCL_OK) {
	return NULL;
    }
    procPtr = *procPtrPtr;
    procPtr->cmdPtr = NULL;

    if (iPtr->cmdFramePtr) {
	CmdFrame context = *iPtr->cmdFramePtr;

	if (context.type == TCL_LOCATION_BC) {
	    /*
	     * Retrieve source information from the bytecode, if possible. If
	     * the information is retrieved successfully, context.type will be
	     * TCL_LOCATION_SOURCE and the reference held by
	     * context.data.eval.path will be counted.
	     */

	    TclGetSrcInfoForPc(&context);
	} else if (context.type == TCL_LOCATION_SOURCE) {
	    /*
	     * The copy into 'context' up above has created another reference
	     * to 'context.data.eval.path'; account for it.
	     */

	    Tcl_IncrRefCount(context.data.eval.path);
	}

	if (context.type == TCL_LOCATION_SOURCE) {
	    /*
	     * We can account for source location within a proc only if the
	     * proc body was not created by substitution.
	     * (FIXME: check that this is sane and correct!)
	     */

	    if (context.line
		    && (context.nline >= 4) && (context.line[3] >= 0)) {
		int isNew;
		CmdFrame *cfPtr = (CmdFrame *) ckalloc(sizeof(CmdFrame));
		Tcl_HashEntry *hPtr;

		cfPtr->level = -1;
		cfPtr->type = context.type;
		cfPtr->line = (int *) ckalloc(sizeof(int));
		cfPtr->line[0] = context.line[3];
		cfPtr->nline = 1;
		cfPtr->framePtr = NULL;
		cfPtr->nextPtr = NULL;

		cfPtr->data.eval.path = context.data.eval.path;
		Tcl_IncrRefCount(cfPtr->data.eval.path);

		cfPtr->cmd.str.cmd = NULL;
		cfPtr->cmd.str.len = 0;

		hPtr = Tcl_CreateHashEntry(iPtr->linePBodyPtr,
			(char *) procPtr, &isNew);
		Tcl_SetHashValue(hPtr, cfPtr);
	    }

	    /*
	     * 'context' is going out of scope; account for the reference that
	     * it's holding to the path name.
	     */

	    Tcl_DecrRefCount(context.data.eval.path);
	    context.data.eval.path = NULL;
	}
    }

    return Tcl_NewInstanceMethod(interp, (Tcl_Object) oPtr, nameObj, flags,
	    typePtr, clientData);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOMakeProcMethod --
 *
 *	The guts of the code to make a procedure-like method for a class.
 *	Split apart so that it is easier for other extensions to reuse (in
 *	particular, it frees them from having to pry so deeply into Tcl's
 *	guts).
 *
 * ----------------------------------------------------------------------
 */

Tcl_Method
TclOOMakeProcMethod(
    Tcl_Interp *interp,		/* The interpreter containing the class. */
    Class *clsPtr,		/* The class to modify. */
    int flags,			/* Whether this is a public method. */
    Tcl_Obj *nameObj,		/* The name of the method, which may be NULL;
				 * if so, up to caller to manage storage
				 * (e.g., because it is a constructor or
				 * destructor). */
    const char *namePtr,	/* The name of the method as a string, which
				 * _must not_ be NULL. */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which _must not_ be NULL. */
    Tcl_Obj *bodyObj,		/* The body of the method, which _must not_ be
				 * NULL. */
    const Tcl_MethodType *typePtr,
				/* The type of the method to create. */
    ClientData clientData,	/* The per-method type-specific data. */
    Proc **procPtrPtr)		/* A pointer to the variable in which to write
				 * the procedure record reference. Presumably
				 * inside the structure indicated by the
				 * pointer in clientData. */
{
    Interp *iPtr = (Interp *) interp;
    Proc *procPtr;

    if (TclCreateProc(interp, NULL, namePtr, argsObj, bodyObj,
	    procPtrPtr) != TCL_OK) {
	return NULL;
    }
    procPtr = *procPtrPtr;
    procPtr->cmdPtr = NULL;

    if (iPtr->cmdFramePtr) {
	CmdFrame context = *iPtr->cmdFramePtr;

	if (context.type == TCL_LOCATION_BC) {
	    /*
	     * Retrieve source information from the bytecode, if possible. If
	     * the information is retrieved successfully, context.type will be
	     * TCL_LOCATION_SOURCE and the reference held by
	     * context.data.eval.path will be counted.
	     */

	    TclGetSrcInfoForPc(&context);
	} else if (context.type == TCL_LOCATION_SOURCE) {
	    /*
	     * The copy into 'context' up above has created another reference
	     * to 'context.data.eval.path'; account for it.
	     */

	    Tcl_IncrRefCount(context.data.eval.path);
	}

	if (context.type == TCL_LOCATION_SOURCE) {
	    /*
	     * We can account for source location within a proc only if the
	     * proc body was not created by substitution.
	     * (FIXME: check that this is sane and correct!)
	     */

	    if (context.line
		    && (context.nline >= 4) && (context.line[3] >= 0)) {
		int isNew;
		CmdFrame *cfPtr = (CmdFrame *) ckalloc(sizeof(CmdFrame));
		Tcl_HashEntry *hPtr;

		cfPtr->level = -1;
		cfPtr->type = context.type;
		cfPtr->line = (int *) ckalloc(sizeof(int));
		cfPtr->line[0] = context.line[3];
		cfPtr->nline = 1;
		cfPtr->framePtr = NULL;
		cfPtr->nextPtr = NULL;

		cfPtr->data.eval.path = context.data.eval.path;
		Tcl_IncrRefCount(cfPtr->data.eval.path);

		cfPtr->cmd.str.cmd = NULL;
		cfPtr->cmd.str.len = 0;

		hPtr = Tcl_CreateHashEntry(iPtr->linePBodyPtr,
			(char *) procPtr, &isNew);
		Tcl_SetHashValue(hPtr, cfPtr);
	    }

	    /*
	     * 'context' is going out of scope; account for the reference that
	     * it's holding to the path name.
	     */

	    Tcl_DecrRefCount(context.data.eval.path);
	    context.data.eval.path = NULL;
	}
    }

    return Tcl_NewMethod(interp, (Tcl_Class) clsPtr, nameObj, flags, typePtr,
	    clientData);
}

/*
 * ----------------------------------------------------------------------
 *
 * InvokeProcedureMethod, PushMethodCallFrame --
 *
 *	How to invoke a procedure-like method.
 *
 * ----------------------------------------------------------------------
 */

static int
InvokeProcedureMethod(
    ClientData clientData,	/* Pointer to some per-method context. */
    Tcl_Interp *interp,
    Tcl_ObjectContext context,	/* The method calling context. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const *objv)	/* Arguments as actually seen. */
{
    ProcedureMethod *pmPtr = clientData;
    int result;
    register int skip;
    PMFrameData *fdPtr;		/* Important data that has to have a lifetime
				 * matched by this function (or rather, by the
				 * call frame's lifetime). */

    /*
     * Allocate the special frame data.
     */

    fdPtr = (PMFrameData *) TclStackAlloc(interp, sizeof(PMFrameData));
    Tcl_Preserve(pmPtr);

    /*
     * Create a call frame for this method.
     */

    result = PushMethodCallFrame(interp, (CallContext *) context, pmPtr,
	    objc, objv, fdPtr);
    if (result != TCL_OK) {
	goto done;
    }

    /*
     * Give the pre-call callback a chance to do some setup and, possibly,
     * veto the call.
     */

    if (pmPtr->preCallProc != NULL) {
	int isFinished;

	result = pmPtr->preCallProc(pmPtr->clientData, interp, context,
		(Tcl_CallFrame *) fdPtr->framePtr, &isFinished);
	if (isFinished || result != TCL_OK) {
	    Tcl_PopCallFrame(interp);
	    TclStackFree(interp, fdPtr->framePtr);
	    goto done;
	}
    }

    /*
     * Now invoke the body of the method. Note that we need to take special
     * action when doing unknown processing to ensure that the missing method
     * name is passed as an argument.
     */

    skip = Tcl_ObjectContextSkippedArgs(context);
    result = TclObjInterpProcCore(interp, fdPtr->nameObj, skip,
	    fdPtr->errProc);

    /*
     * Give the post-call callback a chance to do some cleanup. Note that at
     * this point the call frame itself is invalid; it's already been popped.
     */

    if (pmPtr->postCallProc) {
	result = pmPtr->postCallProc(pmPtr->clientData, interp, context,
		Tcl_GetObjectNamespace(Tcl_ObjectContextObject(context)),
		result);
    }

    /*
     * Scrap the special frame data now that we're done with it.
     */

  done:
    Tcl_Release(pmPtr);
    TclStackFree(interp, fdPtr);
    return result;
}

static int
PushMethodCallFrame(
    Tcl_Interp *interp,		/* Current interpreter. */
    CallContext *contextPtr,	/* Current method call context. */
    ProcedureMethod *pmPtr,	/* Information about this procedure-like
				 * method. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const *objv,	/* Array of arguments. */
    PMFrameData *fdPtr)		/* Place to store information about the call
				 * frame. */
{
    Object *oPtr = contextPtr->callPtr->oPtr;
    Tcl_Namespace *nsPtr = oPtr->namespacePtr;
    int flags = FRAME_IS_METHOD, result;
    const char *namePtr;
    CallFrame **framePtrPtr = &fdPtr->framePtr;
    static Tcl_ObjType *byteCodeTypePtr = NULL;

    /*
     * Compute basic information on the basis of the type of method it is.
     */

    if (contextPtr->callPtr->flags & CONSTRUCTOR) {
	Foundation *fPtr = TclOOGetFoundation(interp);

	namePtr = "<constructor>";
	flags |= FRAME_IS_CONSTRUCTOR;
	fdPtr->nameObj = fPtr->constructorName;
	fdPtr->errProc = ConstructorErrorHandler;
    } else if (contextPtr->callPtr->flags & DESTRUCTOR) {
	Foundation *fPtr = TclOOGetFoundation(interp);

	namePtr = "<destructor>";
	flags |= FRAME_IS_DESTRUCTOR;
	fdPtr->nameObj = fPtr->destructorName;
	fdPtr->errProc = DestructorErrorHandler;
    } else {
	fdPtr->nameObj = Tcl_MethodName(
		Tcl_ObjectContextMethod((Tcl_ObjectContext) contextPtr));
	namePtr = TclGetString(fdPtr->nameObj);
	fdPtr->errProc = MethodErrorHandler;
    }
    if (pmPtr->errProc != NULL) {
	fdPtr->errProc = pmPtr->errProc;
    }

    /*
     * Magic to enable things like [incr Tcl], which wants methods to run in
     * their class's namespace.
     */

    if (pmPtr->flags & USE_DECLARER_NS) {
	register Method *mPtr =
		contextPtr->callPtr->chain[contextPtr->index].mPtr;

	if (mPtr->declaringClassPtr != NULL) {
	    nsPtr = mPtr->declaringClassPtr->thisPtr->namespacePtr;
	} else {
	    nsPtr = mPtr->declaringObjectPtr->namespacePtr;
	}
    }

    /*
     * Compile the body. This operation may fail.
     */

    fdPtr->efi.length = 2;
    memset(&fdPtr->cmd, 0, sizeof(Command));
    fdPtr->cmd.nsPtr = (Namespace *) nsPtr;
    fdPtr->cmd.clientData = &fdPtr->efi;
    pmPtr->procPtr->cmdPtr = &fdPtr->cmd;

    /* Should be a reference to tclByteCodeType, but that's MODULE_SCOPE */
    if (byteCodeTypePtr == NULL ||
	    pmPtr->procPtr->bodyPtr->typePtr != byteCodeTypePtr) {
	result = TclProcCompileProc(interp, pmPtr->procPtr,
		pmPtr->procPtr->bodyPtr, (Namespace *) nsPtr,
		"body of method", namePtr);
	if (result != TCL_OK) {
	    return result;
	}
	if (byteCodeTypePtr == NULL) {
	    byteCodeTypePtr = pmPtr->procPtr->bodyPtr->typePtr;
	}
    }

    /*
     * Make the stack frame and fill it out with information about this call.
     * This operation may fail.
     */

    flags |= FRAME_IS_PROC;
    result = TclPushStackFrame(interp, (Tcl_CallFrame **) framePtrPtr, nsPtr,
	    flags);
    if (result != TCL_OK) {
	return result;
    }

    fdPtr->framePtr->clientData = contextPtr;
    fdPtr->framePtr->objc = objc;
    fdPtr->framePtr->objv = objv;
    fdPtr->framePtr->procPtr = pmPtr->procPtr;

    /*
     * Finish filling out the extra frame info so that [info frame] works.
     */

    fdPtr->efi.fields[0].name = "method";
    fdPtr->efi.fields[0].proc = NULL;
    fdPtr->efi.fields[0].clientData = fdPtr->nameObj;
    if (pmPtr->gfivProc != NULL) {
	fdPtr->efi.fields[1].proc = pmPtr->gfivProc;
	fdPtr->efi.fields[1].clientData = pmPtr;
    } else {
	register Tcl_Method method =
		Tcl_ObjectContextMethod((Tcl_ObjectContext) contextPtr);

	if (Tcl_MethodDeclarerObject(method) != NULL) {
	    fdPtr->efi.fields[1].name = "object";
	} else {
	    fdPtr->efi.fields[1].name = "class";
	}
	fdPtr->efi.fields[1].proc = RenderDeclarerName;
	fdPtr->efi.fields[1].clientData = &fdPtr->pni;
	fdPtr->pni.interp = interp;
	fdPtr->pni.method = method;
    }

    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * RenderDeclarerName --
 *
 *	Returns the name of the entity (object or class) which declared a
 *	method. Used for producing information for [info frame] in such a way
 *	that the expensive part of this (generating the object or class name
 *	itself) isn't done until it is needed.
 *
 * ----------------------------------------------------------------------
 */

static Tcl_Obj *
RenderDeclarerName(
    ClientData clientData)
{
    struct PNI *pni = clientData;
    Tcl_Object object = Tcl_MethodDeclarerObject(pni->method);

    if (object == NULL) {
	object = Tcl_GetClassAsObject(Tcl_MethodDeclarerClass(pni->method));
    }
    return TclOOObjectName(pni->interp, (Object *) object);
}

/*
 * ----------------------------------------------------------------------
 *
 * MethodErrorHandler, ConstructorErrorHandler, DestructorErrorHandler --
 *
 *	How to fill in the stack trace correctly upon error in various forms
 *	of procedure-like methods. LIMIT is how long the inserted strings in
 *	the error traces should get before being converted to have ellipses,
 *	and ELLIPSIFY is a macro to do the conversion (with the help of a
 *	%.*s%s format field). Note that ELLIPSIFY is only safe for use in
 *	suitable formatting contexts.
 *
 * ----------------------------------------------------------------------
 */

#define LIMIT 60
#define ELLIPSIFY(str,len) \
	((len) > LIMIT ? LIMIT : (len)), (str), ((len) > LIMIT ? "..." : "")

static void
MethodErrorHandler(
    Tcl_Interp *interp,
    Tcl_Obj *methodNameObj)
{
    int nameLen, objectNameLen;
    CallContext *contextPtr = ((Interp *) interp)->varFramePtr->clientData;
    Method *mPtr = contextPtr->callPtr->chain[contextPtr->index].mPtr;
    const char *objectName, *kindName, *methodName =
	    Tcl_GetStringFromObj(mPtr->namePtr, &nameLen);
    Object *declarerPtr;

    if (mPtr->declaringObjectPtr != NULL) {
	declarerPtr = mPtr->declaringObjectPtr;
	kindName = "object";
    } else {
	if (mPtr->declaringClassPtr == NULL) {
	    Tcl_Panic("method not declared in class or object");
	}
	declarerPtr = mPtr->declaringClassPtr->thisPtr;
	kindName = "class";
    }

    objectName = Tcl_GetStringFromObj(TclOOObjectName(interp, declarerPtr),
	    &objectNameLen);
    Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
	    "\n    (%s \"%.*s%s\" method \"%.*s%s\" line %d)",
	    kindName, ELLIPSIFY(objectName, objectNameLen),
	    ELLIPSIFY(methodName, nameLen), interp->errorLine));
}

static void
ConstructorErrorHandler(
    Tcl_Interp *interp,
    Tcl_Obj *methodNameObj)
{
    CallContext *contextPtr = ((Interp *) interp)->varFramePtr->clientData;
    Method *mPtr = contextPtr->callPtr->chain[contextPtr->index].mPtr;
    Object *declarerPtr;
    const char *objectName, *kindName;
    int objectNameLen;

    if (interp->errorLine == 0xDEADBEEF) {
	/*
	 * Horrible hack to deal with certain constructors that must not add
	 * information to the error trace.
	 */

	return;
    }

    if (mPtr->declaringObjectPtr != NULL) {
	declarerPtr = mPtr->declaringObjectPtr;
	kindName = "object";
    } else {
	if (mPtr->declaringClassPtr == NULL) {
	    Tcl_Panic("method not declared in class or object");
	}
	declarerPtr = mPtr->declaringClassPtr->thisPtr;
	kindName = "class";
    }

    objectName = Tcl_GetStringFromObj(TclOOObjectName(interp, declarerPtr),
	    &objectNameLen);
    Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
	    "\n    (%s \"%.*s%s\" constructor line %d)", kindName,
	    ELLIPSIFY(objectName, objectNameLen), interp->errorLine));
}

static void
DestructorErrorHandler(
    Tcl_Interp *interp,
    Tcl_Obj *methodNameObj)
{
    CallContext *contextPtr = ((Interp *) interp)->varFramePtr->clientData;
    Method *mPtr = contextPtr->callPtr->chain[contextPtr->index].mPtr;
    Object *declarerPtr;
    const char *objectName, *kindName;
    int objectNameLen;

    if (mPtr->declaringObjectPtr != NULL) {
	declarerPtr = mPtr->declaringObjectPtr;
	kindName = "object";
    } else {
	if (mPtr->declaringClassPtr == NULL) {
	    Tcl_Panic("method not declared in class or object");
	}
	declarerPtr = mPtr->declaringClassPtr->thisPtr;
	kindName = "class";
    }

    objectName = Tcl_GetStringFromObj(TclOOObjectName(interp, declarerPtr),
	    &objectNameLen);
    Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
	    "\n    (%s \"%.*s%s\" destructor line %d)", kindName,
	    ELLIPSIFY(objectName, objectNameLen), interp->errorLine));
}

/*
 * ----------------------------------------------------------------------
 *
 * DeleteProcedureMethod, CloneProcedureMethod --
 *
 *	How to delete and clone procedure-like methods.
 *
 * ----------------------------------------------------------------------
 */

static void
DeleteProcedureMethodRecord(
    char *recordPtr)
{
    register ProcedureMethod *pmPtr = (ProcedureMethod *) recordPtr;

    TclProcDeleteProc(pmPtr->procPtr);
    if (pmPtr->deleteClientdataProc) {
	pmPtr->deleteClientdataProc(pmPtr->clientData);
    }
    ckfree(recordPtr);
}

static void
DeleteProcedureMethod(
    ClientData clientData)
{
    Tcl_EventuallyFree(clientData, DeleteProcedureMethodRecord);
}

static int
CloneProcedureMethod(
    Tcl_Interp *interp,
    ClientData clientData,
    ClientData *newClientData)
{
    ProcedureMethod *pmPtr = clientData;
    ProcedureMethod *pm2Ptr = (ProcedureMethod *)
	    ckalloc(sizeof(ProcedureMethod));

    memcpy(pm2Ptr, pmPtr, sizeof(ProcedureMethod));
    pm2Ptr->procPtr->refCount++;
    if (pmPtr->cloneClientdataProc) {
	pm2Ptr->clientData = pmPtr->cloneClientdataProc(pmPtr->clientData);
    }
    *newClientData = pm2Ptr;
    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewForwardMethod --
 *
 *	Create a forwarded method for an object.
 *
 * ----------------------------------------------------------------------
 */

Method *
TclOONewForwardInstanceMethod(
    Tcl_Interp *interp,		/* Interpreter for error reporting. */
    Object *oPtr,		/* The object to attach the method to. */
    int flags,			/* Whether the method is public or not. */
    Tcl_Obj *nameObj,		/* The name of the method. */
    Tcl_Obj *prefixObj)		/* List of arguments that form the command
				 * prefix to forward to. */
{
    int prefixLen;
    register ForwardMethod *fmPtr;

    if (Tcl_ListObjLength(interp, prefixObj, &prefixLen) != TCL_OK) {
	return NULL;
    }
    if (prefixLen < 1) {
	Tcl_AppendResult(interp, "method forward prefix must be non-empty",
		NULL);
	return NULL;
    }

    fmPtr = (ForwardMethod *) ckalloc(sizeof(ForwardMethod));
    fmPtr->prefixObj = prefixObj;
    Tcl_IncrRefCount(prefixObj);
    return (Method *) Tcl_NewInstanceMethod(interp, (Tcl_Object) oPtr,
	    nameObj, flags, &fwdMethodType, fmPtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOONewForwardMethod --
 *
 *	Create a new forwarded method for a class.
 *
 * ----------------------------------------------------------------------
 */

Method *
TclOONewForwardMethod(
    Tcl_Interp *interp,		/* Interpreter for error reporting. */
    Class *clsPtr,		/* The class to attach the method to. */
    int flags,			/* Whether the method is public or not. */
    Tcl_Obj *nameObj,		/* The name of the method. */
    Tcl_Obj *prefixObj)		/* List of arguments that form the command
				 * prefix to forward to. */
{
    int prefixLen;
    register ForwardMethod *fmPtr;

    if (Tcl_ListObjLength(interp, prefixObj, &prefixLen) != TCL_OK) {
	return NULL;
    }
    if (prefixLen < 1) {
	Tcl_AppendResult(interp, "method forward prefix must be non-empty",
		NULL);
	return NULL;
    }

    fmPtr = (ForwardMethod *) ckalloc(sizeof(ForwardMethod));
    fmPtr->prefixObj = prefixObj;
    Tcl_IncrRefCount(prefixObj);
    return (Method *) Tcl_NewMethod(interp, (Tcl_Class) clsPtr, nameObj,
	    flags, &fwdMethodType, fmPtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * InvokeForwardMethod --
 *
 *	How to invoke a forwarded method. Works by doing some ensemble-like
 *	command rearranging and then invokes some other Tcl command.
 *
 * ----------------------------------------------------------------------
 */

static int
InvokeForwardMethod(
    ClientData clientData,	/* Pointer to some per-method context. */
    Tcl_Interp *interp,
    Tcl_ObjectContext context,	/* The method calling context. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const *objv)	/* Arguments as actually seen. */
{
    CallContext *contextPtr = (CallContext *) context;
    ForwardMethod *fmPtr = clientData;
    Tcl_Obj **argObjs, **prefixObjs;
    int numPrefixes, result, len, skip = contextPtr->skip;

    /*
     * Build the real list of arguments to use. Note that we know that the
     * prefixObj field of the ForwardMethod structure holds a reference to a
     * non-empty list, so there's a whole class of failures ("not a list") we
     * can ignore here.
     */

    Tcl_ListObjGetElements(NULL, fmPtr->prefixObj, &numPrefixes, &prefixObjs);
    argObjs = InitEnsembleRewrite(interp, objc, objv, skip,
	    numPrefixes, prefixObjs, &len);

    result = Tcl_EvalObjv(interp, len, argObjs, TCL_EVAL_INVOKE);
    TclStackFree(interp, argObjs);
    return result;
}

/*
 * ----------------------------------------------------------------------
 *
 * DeleteForwardMethod, CloneForwardMethod --
 *
 *	How to delete and clone forwarded methods.
 *
 * ----------------------------------------------------------------------
 */

static void
DeleteForwardMethod(
    ClientData clientData)
{
    ForwardMethod *fmPtr = clientData;

    Tcl_DecrRefCount(fmPtr->prefixObj);
    ckfree((char *) fmPtr);
}

static int
CloneForwardMethod(
    Tcl_Interp *interp,
    ClientData clientData,
    ClientData *newClientData)
{
    ForwardMethod *fmPtr = clientData;
    ForwardMethod *fm2Ptr = (ForwardMethod *) ckalloc(sizeof(ForwardMethod));

    fm2Ptr->prefixObj = fmPtr->prefixObj;
    Tcl_IncrRefCount(fm2Ptr->prefixObj);
    *newClientData = fm2Ptr;
    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOGetProcFromMethod, TclOOGetFwdFromMethod --
 *
 *	Utility functions used for procedure-like and forwarding method
 *	introspection.
 *
 * ----------------------------------------------------------------------
 */

Proc *
TclOOGetProcFromMethod(
    Method *mPtr)
{
    if (mPtr->typePtr == &procMethodType) {
	ProcedureMethod *pmPtr = mPtr->clientData;

	return pmPtr->procPtr;
    }
    return NULL;
}

Tcl_Obj *
TclOOGetFwdFromMethod(
    Method *mPtr)
{
    if (mPtr->typePtr == &fwdMethodType) {
	ForwardMethod *fwPtr = mPtr->clientData;

	return fwPtr->prefixObj;
    }
    return NULL;
}

/*
 * ----------------------------------------------------------------------
 *
 * InitEnsembleRewrite --
 *
 *	Utility function that wraps up a lot of the complexity involved in
 *	doing ensemble-like command forwarding. Here is a picture of memory
 *	management plan:
 *
 *                    <-----------------objc---------------------->
 *      objv:        |=============|===============================|
 *                    <-toRewrite->           |
 *                                             \
 *                    <-rewriteLength->         \
 *      rewriteObjs: |=================|         \
 *                           |                    |
 *                           V                    V
 *      argObjs:     |=================|===============================|
 *                    <------------------*lengthPtr------------------->
 *
 * ----------------------------------------------------------------------
 */

static Tcl_Obj **
InitEnsembleRewrite(
    Tcl_Interp *interp,		/* Place to log the rewrite info. */
    int objc,			/* Number of real arguments. */
    Tcl_Obj *const *objv,	/* The real arguments. */
    int toRewrite,		/* Number of real arguments to replace. */
    int rewriteLength,		/* Number of arguments to insert instead. */
    Tcl_Obj *const *rewriteObjs,/* Arguments to insert instead. */
    int *lengthPtr)		/* Where to write the resulting length of the
				 * array of rewritten arguments. */
{
    Interp *iPtr = (Interp *) interp;
    int isRootEnsemble = (iPtr->ensembleRewrite.sourceObjs == NULL);
    Tcl_Obj **argObjs;
    unsigned len = rewriteLength + objc - toRewrite;

    argObjs = TclStackAlloc(interp, sizeof(Tcl_Obj *) * len);
    memcpy(argObjs, rewriteObjs, rewriteLength * sizeof(Tcl_Obj *));
    memcpy(argObjs + rewriteLength, objv + toRewrite,
	    sizeof(Tcl_Obj *) * (objc - toRewrite));

    /*
     * Now plumb this into the core ensemble rewrite logging system so that
     * Tcl_WrongNumArgs() can rewrite its result appropriately. The rules for
     * how to store the rewrite rules get complex solely because of the case
     * where an ensemble rewrites itself out of the picture; when that
     * happens, the quality of the error message rewrite falls drastically
     * (and unavoidably).
     */

    if (isRootEnsemble) {
	iPtr->ensembleRewrite.sourceObjs = objv;
	iPtr->ensembleRewrite.numRemovedObjs = toRewrite;
	iPtr->ensembleRewrite.numInsertedObjs = rewriteLength;
    } else {
	int numIns = iPtr->ensembleRewrite.numInsertedObjs;

	if (numIns < toRewrite) {
	    iPtr->ensembleRewrite.numRemovedObjs += toRewrite - numIns;
	    iPtr->ensembleRewrite.numInsertedObjs += rewriteLength - 1;
	} else {
	    iPtr->ensembleRewrite.numInsertedObjs +=
		    rewriteLength - toRewrite;
	}
    }

    *lengthPtr = len;
    return argObjs;
}

/*
 * ----------------------------------------------------------------------
 *
 * assorted trivial 'getter' functions
 *
 * ----------------------------------------------------------------------
 */

Tcl_Object
Tcl_MethodDeclarerObject(
    Tcl_Method method)
{
    return (Tcl_Object) ((Method *) method)->declaringObjectPtr;
}

Tcl_Class
Tcl_MethodDeclarerClass(
    Tcl_Method method)
{
    return (Tcl_Class) ((Method *) method)->declaringClassPtr;
}

Tcl_Obj *
Tcl_MethodName(
    Tcl_Method method)
{
    return ((Method *) method)->namePtr;
}

int
Tcl_MethodIsType(
    Tcl_Method method,
    const Tcl_MethodType *typePtr,
    ClientData *clientDataPtr)
{
    Method *mPtr = (Method *) method;

    if (mPtr->typePtr == typePtr) {
	if (clientDataPtr != NULL) {
	    *clientDataPtr = mPtr->clientData;
	}
	return 1;
    }
    return 0;
}

int
Tcl_MethodIsPublic(
    Tcl_Method method)
{
    return (((Method *)method)->flags & PUBLIC_METHOD) ? 1 : 0;
}

/*
 * Extended method construction for itcl-ng.
 */

Tcl_Method
TclOONewProcInstanceMethodEx(
    Tcl_Interp *interp,		/* The interpreter containing the object. */
    Tcl_Object oPtr,		/* The object to modify. */
    TclOO_PreCallProc preCallPtr,
    TclOO_PostCallProc postCallPtr,
    ProcErrorProc errProc,
    ClientData clientData,
    Tcl_Obj *nameObj,		/* The name of the method, which must not be
				 * NULL. */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which must not be NULL. */
    Tcl_Obj *bodyObj,		/* The body of the method, which must not be
				 * NULL. */
    int flags,			/* Whether this is a public method. */
    void **internalTokenPtr)	/* If non-NULL, points to a variable that gets
				 * the reference to the ProcedureMethod
				 * structure. */
{
    ProcedureMethod *pmPtr;
    Tcl_Method method = (Tcl_Method) TclOONewProcInstanceMethod(interp,
	    (Object *) oPtr, flags, nameObj, argsObj, bodyObj, &pmPtr);

    if (method == NULL) {
	return NULL;
    }
    pmPtr->flags = flags & USE_DECLARER_NS;
    pmPtr->preCallProc = preCallPtr;
    pmPtr->postCallProc = postCallPtr;
    pmPtr->errProc = errProc;
    pmPtr->clientData = clientData;
    if (internalTokenPtr != NULL) {
	*internalTokenPtr = pmPtr;
    }
    return method;
}

Tcl_Method
TclOONewProcMethodEx(
    Tcl_Interp *interp,		/* The interpreter containing the class. */
    Tcl_Class clsPtr,		/* The class to modify. */
    TclOO_PreCallProc preCallPtr,
    TclOO_PostCallProc postCallPtr,
    ProcErrorProc errProc,
    ClientData clientData,
    Tcl_Obj *nameObj,		/* The name of the method, which may be NULL;
				 * if so, up to caller to manage storage
				 * (e.g., because it is a constructor or
				 * destructor). */
    Tcl_Obj *argsObj,		/* The formal argument list for the method,
				 * which may be NULL; if so, it is equivalent
				 * to an empty list. */
    Tcl_Obj *bodyObj,		/* The body of the method, which must not be
				 * NULL. */
    int flags,			/* Whether this is a public method. */
    void **internalTokenPtr)	/* If non-NULL, points to a variable that gets
				 * the reference to the ProcedureMethod
				 * structure. */
{
    ProcedureMethod *pmPtr;
    Tcl_Method method = (Tcl_Method) TclOONewProcMethod(interp,
	    (Class *) clsPtr, flags, nameObj, argsObj, bodyObj, &pmPtr);

    if (method == NULL) {
	return NULL;
    }
    pmPtr->flags = flags & USE_DECLARER_NS;
    pmPtr->preCallProc = preCallPtr;
    pmPtr->postCallProc = postCallPtr;
    pmPtr->errProc = errProc;
    pmPtr->clientData = clientData;
    if (internalTokenPtr != NULL) {
	*internalTokenPtr = pmPtr;
    }
    return method;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */

;                             ****************
;                             *              *
;                             * LEGOLOGO.CTL *
;                             *              *
;*************************************************************************
;*                                                                       *
;* This CYBER CONTROL script creates a cartoon of a rotating LEGO logo.  *
;* LEGO bricks were modeled using CYBER SCULPT and uploaded into CAD-3D. *
;* The delta animation produced by this script was then loaded into      *
;* CYBER PAINT, where borders and text were added.  The animation was    *
;* then saved as a sequence file, drastically reducing its size.  If you *
;* use this script or the CAD-3D objects in LEGOLOGO.3D2 for you own     *
;* purposes, please cite me as the originator.                           *
;*                                                                       *
;* Written by Ray Perry on August 17, 1989.  For more information, call  *
;* (314) 469-1214, or send electronic mail to R.PERRY6 on GEnie.         *
;*                                                                       *
;*************************************************************************
;
; load objects from disk
;
LOAD3D "F:\CYBER\LEGOLOGO.3D2"
;
; set viewing parameters
;
VIEW SOLID:FINAL:MONO
CAM1 0,10,0
ZOOM 420
PERSPEC 999
WATCH ON
;
; start recording
;
RSTART "F:\CYBER\LEGOLOGO",M
;
; move everything but the base upward
; and out of the camera field of view
;
ALLGRP
DESELECT BASE
XLATE 0,0,800
CLRGRP
;
; set center of rotation to 0,0,0
;
CENTER UNIVERSE
;
;
; build the white letter "L"
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 8WHITE1
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2WHITE1
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2WHITE2
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2WHITE3
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2WHITE4
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
;
; build the red letter "E"
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 8RED1
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2RED1
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 8RED2
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2RED2
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 8RED3
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
;
; build the blue letter "G"
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 8BLUE1
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2BLUE1
    SELECT 2BLUE2
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2BLUE3
    SELECT 4BLUE1
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2BLUE4
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 8BLUE2
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
;
; build the yellow letter "O"
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 8YELL1
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2YELL1
    SELECT 2YELL2
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2YELL3
    SELECT 2YELL4
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 2YELL5
    SELECT 2YELL6
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
FOR Z=1 TO 8
    CLRGRP
    SELECT 8YELL2
    XLATE 0,0,-100
    ALLGRP
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
;
; continue rotation until in front view
;
FOR Z=1 TO 20
    ROTATE 0,0,2
    SUPERVIEW
    RECORD
NEXT Z
;
;
; stop recording
;
RSTOP
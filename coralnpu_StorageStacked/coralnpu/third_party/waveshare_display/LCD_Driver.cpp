/*****************************************************************************
* | File      	:	LCD_Driver.c
* | Author      :   Waveshare team
* | Function    :   LCD driver
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2018-12-18
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "LCD_Driver.h"
#include <string.h>
/*******************************************************************************
function:
	Hardware reset
*******************************************************************************/
static void LCD_Reset(void* ctx)
{
	DEV_Delay_ms(ctx, 200);
	DEV_Digital_Write(ctx, DEV_RST_PIN, 0);
	DEV_Delay_ms(ctx, 200);
	DEV_Digital_Write(ctx, DEV_RST_PIN, 1);
	DEV_Delay_ms(ctx, 200);
}

/*******************************************************************************
function:
		Write data and commands
*******************************************************************************/
static void LCD_Write_Command(void* ctx, UBYTE data)	 
{	
	DEV_Digital_Write(ctx, DEV_CS_PIN, 0);
	DEV_Digital_Write(ctx, DEV_DC_PIN, 0);
	DEV_SPI_WRITE(ctx, data);
}

static void LCD_WriteData_Byte(void* ctx, UBYTE data) 
{	
	DEV_Digital_Write(ctx, DEV_CS_PIN, 0);
	DEV_Digital_Write(ctx, DEV_DC_PIN, 1);
	DEV_SPI_WRITE(ctx, data);  
	DEV_Digital_Write(ctx, DEV_CS_PIN,1);
}  

void LCD_WriteData_Word(void* ctx, UWORD data)
{
	DEV_Digital_Write(ctx, DEV_CS_PIN, 0);
	DEV_Digital_Write(ctx, DEV_DC_PIN, 1);
	DEV_SPI_WRITE(ctx, (data>>8) & 0xff);
	DEV_SPI_WRITE(ctx, data);
	DEV_Digital_Write(ctx, DEV_CS_PIN, 1);
}	  


/******************************************************************************
function:	
		Common register initialization
******************************************************************************/
void LCD_Init(void* ctx)
{
	LCD_Reset(ctx);

	LCD_Write_Command(ctx, 0x36);
	LCD_WriteData_Byte(ctx, 0xA0); 

	LCD_Write_Command(ctx, 0x3A); 
	LCD_WriteData_Byte(ctx, 0x05);

	LCD_Write_Command(ctx, 0x21); 

	LCD_Write_Command(ctx, 0x2A);
	LCD_WriteData_Byte(ctx, 0x00);
	LCD_WriteData_Byte(ctx, 0x01);
	LCD_WriteData_Byte(ctx, 0x00);
	LCD_WriteData_Byte(ctx, 0x3F);

	LCD_Write_Command(ctx, 0x2B);
	LCD_WriteData_Byte(ctx, 0x00);
	LCD_WriteData_Byte(ctx, 0x00);
	LCD_WriteData_Byte(ctx, 0x00);
	LCD_WriteData_Byte(ctx, 0xEF);

	LCD_Write_Command(ctx, 0xB2);
	LCD_WriteData_Byte(ctx, 0x0C);
	LCD_WriteData_Byte(ctx, 0x0C);
	LCD_WriteData_Byte(ctx, 0x00);
	LCD_WriteData_Byte(ctx, 0x33);
	LCD_WriteData_Byte(ctx, 0x33);

	LCD_Write_Command(ctx, 0xB7);
	LCD_WriteData_Byte(ctx, 0x35); 

	LCD_Write_Command(ctx, 0xBB);
	LCD_WriteData_Byte(ctx, 0x1F);

	LCD_Write_Command(ctx, 0xC0);
	LCD_WriteData_Byte(ctx, 0x2C);

	LCD_Write_Command(ctx, 0xC2);
	LCD_WriteData_Byte(ctx, 0x01);

	LCD_Write_Command(ctx, 0xC3);
	LCD_WriteData_Byte(ctx, 0x12);   

	LCD_Write_Command(ctx, 0xC4);
	LCD_WriteData_Byte(ctx, 0x20);

	LCD_Write_Command(ctx, 0xC6);
	LCD_WriteData_Byte(ctx, 0x0F); 

	LCD_Write_Command(ctx, 0xD0);
	LCD_WriteData_Byte(ctx, 0xA4);
	LCD_WriteData_Byte(ctx, 0xA1);

	LCD_Write_Command(ctx, 0xE0);
	LCD_WriteData_Byte(ctx, 0xD0);
	LCD_WriteData_Byte(ctx, 0x08);
	LCD_WriteData_Byte(ctx, 0x11);
	LCD_WriteData_Byte(ctx, 0x08);
	LCD_WriteData_Byte(ctx, 0x0C);
	LCD_WriteData_Byte(ctx, 0x15);
	LCD_WriteData_Byte(ctx, 0x39);
	LCD_WriteData_Byte(ctx, 0x33);
	LCD_WriteData_Byte(ctx, 0x50);
	LCD_WriteData_Byte(ctx, 0x36);
	LCD_WriteData_Byte(ctx, 0x13);
	LCD_WriteData_Byte(ctx, 0x14);
	LCD_WriteData_Byte(ctx, 0x29);
	LCD_WriteData_Byte(ctx, 0x2D);

	LCD_Write_Command(ctx, 0xE1);
	LCD_WriteData_Byte(ctx, 0xD0);
	LCD_WriteData_Byte(ctx, 0x08);
	LCD_WriteData_Byte(ctx, 0x10);
	LCD_WriteData_Byte(ctx, 0x08);
	LCD_WriteData_Byte(ctx, 0x06);
	LCD_WriteData_Byte(ctx, 0x06);
	LCD_WriteData_Byte(ctx, 0x39);
	LCD_WriteData_Byte(ctx, 0x44);
	LCD_WriteData_Byte(ctx, 0x51);
	LCD_WriteData_Byte(ctx, 0x0B);
	LCD_WriteData_Byte(ctx, 0x16);
	LCD_WriteData_Byte(ctx, 0x14);
	LCD_WriteData_Byte(ctx, 0x2F);
	LCD_WriteData_Byte(ctx, 0x31);
	LCD_Write_Command(ctx, 0x21);

	LCD_Write_Command(ctx, 0x11);

	LCD_Write_Command(ctx, 0x29);
}

/******************************************************************************
function:	Set the cursor position
parameter	:
	  Xstart: 	Start UWORD x coordinate
	  Ystart:	Start UWORD y coordinate
	  Xend  :	End UWORD coordinates
	  Yend  :	End UWORD coordinatesen
******************************************************************************/
void LCD_SetWindow(void* ctx, UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD  Yend)
{ 
	LCD_Write_Command(ctx, 0x2a);
	LCD_WriteData_Byte(ctx, 0x00);
	LCD_WriteData_Byte(ctx, Xstart & 0xff);
	LCD_WriteData_Byte(ctx, (Xend - 1) >> 8);
	LCD_WriteData_Byte(ctx, (Xend - 1) & 0xff);

	LCD_Write_Command(ctx, 0x2b);
	LCD_WriteData_Byte(ctx, 0x00);
	LCD_WriteData_Byte(ctx, Ystart & 0xff);
	LCD_WriteData_Byte(ctx, (Yend - 1) >> 8);
	LCD_WriteData_Byte(ctx, (Yend - 1) & 0xff);

	LCD_Write_Command(ctx, 0x2C);
}

/******************************************************************************
function:	Settings window
parameter	:
	  Xstart: 	Start UWORD x coordinate
	  Ystart:	Start UWORD y coordinate

******************************************************************************/
void LCD_SetCursor(void* ctx, UWORD X, UWORD Y)
{ 
	LCD_Write_Command(ctx, 0x2a);
	LCD_WriteData_Byte(ctx, X >> 8);
	LCD_WriteData_Byte(ctx, X);
	LCD_WriteData_Byte(ctx, X >> 8);
	LCD_WriteData_Byte(ctx, X);

	LCD_Write_Command(ctx, 0x2b);
	LCD_WriteData_Byte(ctx, Y >> 8);
	LCD_WriteData_Byte(ctx, Y);
	LCD_WriteData_Byte(ctx, Y >> 8);
	LCD_WriteData_Byte(ctx, Y);

	LCD_Write_Command(ctx, 0x2C);
}

/******************************************************************************
function:	Clear screen function, refresh the screen to a certain color
parameter	:
	  Color :		The color you want to clear all the screen
******************************************************************************/
void LCD_Clear(void* ctx, UWORD Color)
{
	unsigned int i,j;  	
	LCD_SetWindow(ctx, 0, 0, LCD_WIDTH, LCD_HEIGHT);
	DEV_Digital_Write(ctx, DEV_DC_PIN, 1);
	for(i = 0; i < LCD_WIDTH; i++){
		for(j = 0; j < LCD_HEIGHT; j++){
			DEV_SPI_WRITE(ctx, (Color>>8)&0xff);
			DEV_SPI_WRITE(ctx, Color);
		}
	 }
}

void LCD_ClearToBuffer(void* ctx, uint16_t* buffer)
{
	LCD_SetWindow(ctx, 0, 0, LCD_WIDTH, LCD_HEIGHT);
	DEV_Digital_Write(ctx, DEV_DC_PIN, 1);
	DEV_SPI_BLOCK_WRITE(ctx, (const uint8_t*)buffer, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
}

void LCD_ClearToBufferWindow(void* ctx, uint16_t* buffer, UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend) {
	LCD_SetWindow(ctx, Xstart, Ystart, Xend, Yend);
	DEV_Digital_Write(ctx, DEV_DC_PIN, 1);
	DEV_SPI_BLOCK_WRITE(ctx, (const uint8_t*)buffer, (Xend - Xstart) * (Yend - Ystart) * sizeof(uint16_t));
}

void LCD_ConvertGrayscaleToRGB565(const uint8_t* grayscale, uint16_t* rgb565, size_t size) {
	for (size_t i = 0; i < size; i++) {
		uint8_t y = grayscale[i];
		uint16_t val = ((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3);
		rgb565[i] = (val << 8) | (val >> 8); // Swap for display
	}
}

/******************************************************************************
function:	Refresh a certain area to the same color
parameter	:
	  Xstart: Start UWORD x coordinate
	  Ystart:	Start UWORD y coordinate
	  Xend  :	End UWORD coordinates
	  Yend  :	End UWORD coordinates
	  color :	Set the color
******************************************************************************/
void LCD_ClearWindow(void* ctx, UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend,UWORD color)
{          
	UWORD i,j; 
	LCD_SetWindow(ctx, Xstart, Ystart, Xend-1,Yend-1);
	for(i = Ystart; i <= Yend-1; i++){													   	 	
		for(j = Xstart; j <= Xend-1; j++){
			LCD_WriteData_Word(ctx, color);
		}
	} 					  	    
}

/******************************************************************************
function: Draw a point
parameter	:
	    X	: 	Set the X coordinate
	    Y	:	Set the Y coordinate
	  Color :	Set the color
******************************************************************************/
void LCD_DrawPaint(void* ctx, UWORD x, UWORD y, UWORD Color)
{
	LCD_SetCursor(ctx, x, y);
	LCD_WriteData_Word(ctx, Color); 	    
}
// ===== GUI_Paint.cpp


#define IMAGE_BACKGROUND    WHITE
#define FONT_FOREGROUND     BLACK
#define FONT_BACKGROUND     WHITE

void Paint_NewImage(PAINT* Paint, UWORD Width, UWORD Height, UWORD Rotate, UWORD Color)
{
    Paint->Image = 0;
    Paint->WidthMemory = Width;
    Paint->HeightMemory = Height;
    Paint->Color = Color;    
    Paint->WidthByte = Width;
    Paint->HeightByte = Height;    
    // printf("WidthByte = %d, HeightByte = %d\r\n", Paint->WidthByte, Paint->HeightByte);
   
    Paint->Rotate = Rotate;
    Paint->Mirror = MIRROR_NONE;
    
    if(Rotate == ROTATE_0 || Rotate == ROTATE_180) {
        Paint->Width = Width;
        Paint->Height = Height;
    } else {
        Paint->Width = Height;
        Paint->Height = Width;
    }
}

void Paint_SetPixel(PAINT* Paint, UWORD Xpoint, UWORD Ypoint, UWORD Color)
{
    if(Xpoint > Paint->Width || Ypoint > Paint->Height){
        // Debug("Exceeding display boundaries\r\n");
        return;
    }      
    UWORD X, Y;

    switch(Paint->Rotate) {
    case 0:
        X = Xpoint;
        Y = Ypoint;  
        break;
    case 90:
        X = Paint->WidthMemory - Ypoint - 1;
        Y = Xpoint;
        break;
    case 180:
        X = Paint->WidthMemory - Xpoint - 1;
        Y = Paint->HeightMemory - Ypoint - 1;
        break;
    case 270:
        X = Ypoint;
        Y = Paint->HeightMemory - Xpoint - 1;
        break;

    default:
        return;
    }
    
    switch(Paint->Mirror) {
    case MIRROR_NONE:
        break;
    case MIRROR_HORIZONTAL:
        X = Paint->WidthMemory - X - 1;
        break;
    case MIRROR_VERTICAL:
        Y = Paint->HeightMemory - Y - 1;
        break;
    case MIRROR_ORIGIN:
        X = Paint->WidthMemory - X - 1;
        Y = Paint->HeightMemory - Y - 1;
        break;
    default:
        return;
    }

    // printf("x = %d, y = %d\r\n", X, Y);
    if(X > Paint->WidthMemory || Y > Paint->HeightMemory){
        // Debug("Exceeding display boundaries\r\n");
        return;
    }
    
   // UDOUBLE Addr = X / 8 + Y * Paint->WidthByte;
   // LCD_DrawPaint(X,Y, Color);
   if (Paint->Image) {
       Paint->Image[Y * Paint->WidthMemory + X] = Color;
   } else {
       LCD_DrawPaint(0, X, Y, Color);
   }
}


void Paint_DrawChar(PAINT* Paint, UWORD Xpoint, UWORD Ypoint, const char Acsii_Char,
                    sFONT* Font, UWORD Color_Background, UWORD Color_Foreground)
{
    UWORD Page, Column;

    if (Xpoint > Paint->Width || Ypoint > Paint->Height) {
        // Debug("Paint_DrawChar Input exceeds the normal display range\r\n");
        return;
    }

    uint32_t Char_Offset = (Acsii_Char - ' ') * Font->Height * (Font->Width / 8 + (Font->Width % 8 ? 1 : 0));
    const unsigned char *ptr = &Font->table[Char_Offset];

    for (Page = 0; Page < Font->Height; Page ++ ) {
        for (Column = 0; Column < Font->Width; Column ++ ) {

            //To determine whether the font background color and screen background color is consistent
            if (FONT_BACKGROUND == Color_Background) { //this process is to speed up the scan
                if (*ptr & (0x80 >> (Column % 8)))
                    Paint_SetPixel(Paint, Xpoint + Column, Ypoint + Page, Color_Foreground);
                    // Paint_DrawPoint(Xpoint + Column, Ypoint + Page, Color_Foreground, DOT_PIXEL_DFT, DOT_STYLE_DFT);
            } else {
                if (*ptr & (0x80 >> (Column % 8))) {
                    Paint_SetPixel(Paint, Xpoint + Column, Ypoint + Page, Color_Foreground);
                    // Paint_DrawPoint(Xpoint + Column, Ypoint + Page, Color_Foreground, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                } else {
                    Paint_SetPixel(Paint, Xpoint + Column, Ypoint + Page, Color_Background);
                    // Paint_DrawPoint(Xpoint + Column, Ypoint + Page, Color_Background, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                }
            }
            //One pixel is 8 bits
            if (Column % 8 == 7)
                ptr++;
        }// Write a line
        if (Font->Width % 8 != 0)
            ptr++;
    }// Write all
}

void Paint_DrawString_EN(PAINT* Paint, UWORD Xstart, UWORD Ystart, const char * pString,
                         sFONT* Font, UWORD Color_Background, UWORD Color_Foreground )
{
    UWORD Xpoint = Xstart;
    UWORD Ypoint = Ystart;

    if (Xstart > Paint->Width || Ystart > Paint->Height) {
        // Debug("Paint_DrawString_EN Input exceeds the normal display range\r\n");
        return;
    }

    while (* pString != '\0') {
        //if X direction filled , reposition to(Xstart,Ypoint),Ypoint is Y direction plus the Height of the character
        if ((Xpoint + Font->Width ) > Paint->Width ) {
            Xpoint = Xstart;
            Ypoint += Font->Height;
        }

        // If the Y direction is full, reposition to(Xstart, Ystart)
        if ((Ypoint  + Font->Height ) > Paint->Height ) {
            Xpoint = Xstart;
            Ypoint = Ystart;
        }
        Paint_DrawChar(Paint, Xpoint, Ypoint, * pString, Font, Color_Background, Color_Foreground);

        //The next character of the address
        pString ++;

        //The next word of the abscissa increases the font of the broadband
        Xpoint += Font->Width;
    }
}

from PIL import Image
from epyper.displayCOGProcess import Display
from epyper. displayController import DisplayController
display = DisplayController(Display.EPD_TYPE_270)
im=Image.open('/home/pi/dog.png')
display.displayImg(im)

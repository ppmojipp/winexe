/* Copyright (c): 2002-2005 (Germany): United Internet, 1&1, GMX, Schlund+Partner, Alturo */
function QxToolBarCheckBox(vText,vIcon,vChecked){QxToolBarButton.call(this,vText,vIcon);if(isValid(vChecked)){this.setChecked(vChecked);};};QxToolBarCheckBox.extend(QxToolBarButton,"QxToolBarCheckBox");QxToolBarCheckBox.addProperty({name:"checked",type:Boolean,defaultValue:false});proto._g2=function(e){if(e.isNotLeftButton()){return;};this.setChecked(!this.getChecked());};proto._onmouseover=function(e){this.setState(this.getChecked()?"pressed":"hover");};proto._onmouseout=function(e){this.setState(this.getChecked()?"checked":null);};proto._modifyChecked=function(_b1,_b2,_b3,_b4){switch(this.getState()){case null:this.setState(_b1?"checked":null,_b4);break;case "checked":this.setState(_b1?"pressed":null,_b4);break;case "pressed":if(!_b1){this.setState("hover");};break;case "hover":if(_b1){this.setState("pressed",_b4);};};return true;};
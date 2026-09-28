--エキセントリック·ボーイ
function c16825874.initial_effect(c)
	--synchro custom: the other material is 1 monster in your hand
	local e1=Effect.CreateEffect(c)
	e1:SetType(EFFECT_TYPE_SINGLE)
	e1:SetCode(EFFECT_SYNCHRO_MATERIAL_CUSTOM)
	e1:SetProperty(EFFECT_FLAG_CANNOT_DISABLE+EFFECT_FLAG_UNCOPYABLE)
	e1:SetOperation(c16825874.synop)
	c:RegisterEffect(e1)
	--allow a hand monster to be used as the other material
	local e2=Effect.CreateEffect(c)
	e2:SetType(EFFECT_TYPE_FIELD)
	e2:SetCode(EFFECT_SYNCHRO_MAT_FROM_HAND)
	e2:SetProperty(EFFECT_FLAG_PLAYER_TARGET)
	e2:SetTargetRange(1,0)
	e2:SetValue(c16825874.handval)
	c:RegisterEffect(e2)
	--be material (negate the summoned monster + banish on leave)
	local e3=Effect.CreateEffect(c)
	e3:SetType(EFFECT_TYPE_SINGLE+EFFECT_TYPE_CONTINUOUS)
	e3:SetCode(EVENT_BE_MATERIAL)
	e3:SetProperty(EFFECT_FLAG_CANNOT_DISABLE)
	e3:SetTarget(c16825874.ccon)
	e3:SetOperation(c16825874.cop)
	c:RegisterEffect(e3)
end
-- a hand monster may be material only if its level completes the synchro level
function c16825874.handval(e,mc,c)
	local lv=c:GetLevel()-e:GetHandler():GetLevel()
	return lv>0 and mc:IsCanBeSynchroMaterial(c) and mc:GetLevel()==lv
end
-- modern EFFECT_SYNCHRO_MATERIAL_CUSTOM operation: enforce that the non-tuner
-- is the hand monster (a field monster must not be used in its place)
function c16825874.synop(e,tsg,ntsg,sg,lv,sc,tp)
	if ntsg:IsExists(function(tc) return not tc:IsLocation(LOCATION_HAND) end,1,nil) then
		return false
	end
	return true
end
function c16825874.ccon(e,tp,eg,ep,ev,re,r,rp)
	return e:GetHandler():IsReason(REASON_SYNCHRO)
end
function c16825874.cop(e,tp,eg,ep,ev,re,r,rp)
	local c=e:GetHandler()
	--leave redirect
	local e1=Effect.CreateEffect(c)
	e1:SetType(EFFECT_TYPE_SINGLE)
	e1:SetCode(EFFECT_LEAVE_FIELD_REDIRECT)
	e1:SetValue(LOCATION_REMOVED)
	e1:SetReset(RESET_EVENT+0x7e0000)
	c:GetReasonCard():RegisterEffect(e1)
	--cannot trigger
	local e2=Effect.CreateEffect(c)
	e2:SetType(EFFECT_TYPE_SINGLE)
	e2:SetCode(EFFECT_CANNOT_TRIGGER)
	e2:SetReset(RESET_EVENT+0x1fc0000)
	c:GetReasonCard():RegisterEffect(e2)
	--disable
	local e3=Effect.CreateEffect(c)
	e3:SetType(EFFECT_TYPE_SINGLE)
	e3:SetCode(EFFECT_DISABLE)
	e3:SetReset(RESET_EVENT+0x1fc0000)
	c:GetReasonCard():RegisterEffect(e3)
end
